#pragma once

#include <utility>
#include <memory>
#include <cassert>

#include "Transaction.hh"
#include "versioned_value.hh"

// Transforms keys into arrays. These arrays should be ordered in the same way
// as the keys. Each element of the array addresses one level of the radix tree.
// Currently only constant-length keys are supported.
template <typename K>
class DefaultKeyTransformer {
public:
  static void transform(const K &key, uint8_t *buf);
  static constexpr int buf_size();
};

template <typename K, typename V, typename KeyTransformer = DefaultKeyTransformer<K> >
class RadixTree : public Shared {
public:
  static constexpr uint8_t span = 4;
  static constexpr uint8_t fanout = (1 << span);

  typedef TransactionTid::type version_t;
  typedef versioned_value_struct<V> versioned_value;

private:
  struct tree_node {
    void *children[fanout]; // The children are tree_node* for internal nodes and versioned_value* for leaves
    uint8_t parent_index;
    tree_node *parent;
    version_t version;
    V *value;

    tree_node(): tree_node(nullptr, 0) {}

    tree_node(tree_node *parent, uint8_t parent_index):
      children(),
      parent_index(parent_index),
      parent(parent),
      version(TransactionTid::increment_value),
      value(nullptr) {}
  };

  static constexpr auto ver_insert_bit = TransactionTid::user_bit1;

  static constexpr auto item_put_bit = TransItem::user0_bit << 0;
  static constexpr auto item_remove_bit = TransItem::user0_bit << 1;

  static constexpr auto item_empty_bit = TransItem::user0_bit << 2;

public:
  RadixTree() : transformer(), root() {}

public:
  bool trans_get(const K &key, V &value) {
    void *vv_or_node;
    bool is_vv = get_value(key, vv_or_node);
    if (!is_vv) {
      // not found, add the version of the node to detect inserts
      auto node = static_cast<tree_node *>(vv_or_node);
      auto item = Sto::item(this, node);
      item.template add_read<version_t>(node->version);
      item.add_flags(item_empty_bit);
      return false;
    }

    auto vv = static_cast<versioned_value *>(vv_or_node);
    auto item = Sto::item(this, vv);

    if (item.has_write()) {
      // Return the value directly from the item if this transaction performed a write.
      // XXX: handle removes in same transaction
      value = item.template write_value<V>();
      return true;
    }

    version_t ver = vv->version();
    if (item.has_read() && !TransactionTid::same_version(item.template read_value<version_t>(), ver)) {
      // The version has changed from the last read, this transaction can't possibly complete.
      Sto::abort();
      return false;
    }

    value = atomic_read(vv, ver);
    item.template add_read<version_t>(ver);
    return ver & TransactionTid::valid_bit;
  }

  bool get(const K &key, V &value) {
    void *vv_or_node;
    bool is_vv = get_value(key, vv_or_node);
    if (!is_vv) {
      return false;
    }

    auto vv = static_cast<versioned_value *>(vv_or_node);
    if (!(vv->version() & TransactionTid::valid_bit)) {
      return false;
    }
    version_t ver;
    value = atomic_read(vv, ver);
    return true;
  }

private:
  bool get_value(const K &key, void * &vv_or_node) {
    constexpr int t_key_sz = KeyTransformer::buf_size();
    uint8_t t_key[t_key_sz];
    transformer.transform(key, t_key);

    void *cur = &root;
    for (int i = 0; i < t_key_sz; i++) {
      tree_node *node = static_cast<tree_node*>(cur);
      if (node->children[t_key[i]] == nullptr) {
        vv_or_node = cur;
        return false;
      }
      cur = node->children[t_key[i]];
    }

    vv_or_node = cur;
    return true;
  }

  V atomic_read(versioned_value *vv, version_t &ver) {
    version_t ver2;
    while (true) {
      ver2 = vv->version();
      fence();
      V value = vv->read_value();
      fence();
      ver = vv->version();
      if (ver == ver2) {
        return value;
      }
    }
  }

public:
  void trans_put(const K &key, const V &value) {
    // Add nodes if they don't exist yet.
    auto vv = insert_nodes(key);
    auto item = Sto::item(this, vv);
    item.add_write(value);
    item.add_flags(item_put_bit);
  }

  void put(const K &key, const V &value) {
    auto vv = insert_nodes(key);

    TransactionTid::lock(vv->version());

    version_t ver = vv->version() + TransactionTid::increment_value;
    ver |= TransactionTid::valid_bit;
    ver &= ~ver_insert_bit;
    vv->version() = ver;
    vv->writeable_value() = value;

    TransactionTid::unlock(vv->version());
  }

private:
  versioned_value *insert_nodes(const K &key) {
    constexpr int t_key_sz = KeyTransformer::buf_size();
    uint8_t t_key[t_key_sz];
    transformer.transform(key, t_key);

    void *cur = static_cast<void *>(&root);

    for (int i = 0; i < t_key_sz; i++) {
      tree_node *node = static_cast<tree_node *>(cur);
      if (node->children[t_key[i]] == nullptr) {
        void *new_node;
        if (i == t_key_sz - 1) {
          // insert versioned value with insert bit set
          versioned_value *vv = new versioned_value();
          vv->version() = ver_insert_bit;
          new_node = static_cast<void *>(vv);
        } else {
          // insert tree node
          new_node = static_cast<void *>(new tree_node(node, t_key[i]));
        }

        // TODO: We can probably do this without actually locking, because only
        // range queries and other inserts are affected by this. But then we
        // still have to use CAS to set the pointer.
        TransactionTid::lock(node->version);

        // Even if this transaction aborts, new tree_nodes/versioned_values
        // might end up being used by some other insert. Therefore, we update
        // the node version now.
        if (node->children[t_key[i]] == nullptr) {
          // Increment the version number of the current node.
          TransactionTid::set_version(node->version, node->version + TransactionTid::increment_value);
          // Insert the new node.
          node->children[t_key[i]] = new_node;
        } else {
          // Someone else already inserted the node/value.
          if (i == t_key_sz - 1) {
            delete static_cast<versioned_value *>(new_node);
          } else {
            delete static_cast<tree_node *>(new_node);
          }
        }
        TransactionTid::unlock(node->version);
      }
      cur = node->children[t_key[i]];
    }

    return static_cast<versioned_value *>(cur);
  }

public:
  void trans_remove(const K &key) {
    void *vv_or_node;
    bool is_vv = get_value(key, vv_or_node);
    if (!is_vv) {
      // not found, add the version of the node to detect inserts
      // XXX: this seems a bit weird - makes remove not exactly a blind write
      auto node = static_cast<tree_node *>(vv_or_node);
      auto item = Sto::item(this, node);
      item.template add_read<version_t>(node->version);
      item.add_flags(item_empty_bit);
      return;
    }

    auto vv = static_cast<versioned_value *>(vv_or_node);
    auto item = Sto::item(this, vv);
    item.add_write(true);
    item.add_flags(item_remove_bit);
  }

  void remove(const K &key) {
    void *vv_or_node;
    bool is_vv = get_value(key, vv_or_node);
    if (!is_vv) {
      return;
    }

    auto vv = static_cast<versioned_value *>(vv_or_node);
    TransactionTid::lock(vv->version());

    version_t ver = vv->version() + TransactionTid::increment_value;
    ver &= ~(TransactionTid::valid_bit | ver_insert_bit);
    vv->version() = ver;

    TransactionTid::unlock(vv->version());
  }

  /*
  iterator trans_query(const K &start) {
    return iterator(this, start);
  }

  class iterator {
  public:
    iterator(const RadixTree *tree, const K &start) :
      tree(tree),
      start(start),
      end(end) {}

    bool next() {
      if (cur == nullptr) {
        return false;
      }

      int parent_index = -1;
      while (true) {
        bool child_found = false;
        if (parent_index < 0) {
          for (int i = 0; i < fanout; i++) {
            if (cur->children[i] != nullptr) {
              child_found = true;
              cur = children[i];
            }
          }
        } else {
          cur = cur->parent;
          for (int i = parent_index + 1; i < fanout; i++) {
            if (cur->children[i] != nullptr) {
              child_found = true;
              cur = cur->children[i];
              parent_index = -1;
            }
          }
        }

        if (child_found && cur->value != nullptr) {
          return true;
        } else if (cur == tree->root) {
          cur == nullptr;
          return false;
        } else {
          // go up the tree
          parent_index = cur->parent_index;
        }
      }
    }

  private:
    RadixTree *tree;
    K start;
    tree_node *cur;
  }

  */

  virtual void lock(TransItem& item) {
    versioned_value *vv = item.template key<versioned_value *>();
    TransactionTid::lock(vv->version());
  }

  virtual bool check(const TransItem& item, const Transaction&) {
    auto flags = item.flags();
    if (flags & item_empty_bit) {
      tree_node *node = item.template key<tree_node *>();
      return TransactionTid::same_version(node->version, item.template read_value<version_t>());
    } else {
      versioned_value *vv = item.template key<versioned_value *>();
      return TransactionTid::same_version(vv->version(), item.template read_value<version_t>());
    }
  }

  virtual void install(TransItem& item, const Transaction&) {
    versioned_value *vv = item.template key<versioned_value *>();
    auto new_ver = vv->version() + TransactionTid::increment_value;
    auto flags = item.flags();
    if (flags & item_put_bit) {
      new_ver |= TransactionTid::valid_bit;
      new_ver &= ~ver_insert_bit;
      vv->writeable_value() = item.template write_value<V>();
    } else if (flags & item_remove_bit) {
      new_ver &= ~TransactionTid::valid_bit;
      new_ver &= ~ver_insert_bit;
    }
    TransactionTid::set_version(vv->version(), new_ver);
  }

  virtual void unlock(TransItem& item) {
    versioned_value *vv = item.template key<versioned_value *>();
    TransactionTid::unlock(vv->version());
  }

  /*
  virtual void cleanup(TransItem& item, bool committed) {
  }
  */

private:
  const KeyTransformer transformer;
  tree_node root;
};

template<>
void DefaultKeyTransformer<uint64_t>::transform(const uint64_t &key, uint8_t *buf) {
  for (size_t i = 0; i < sizeof(key)*2; i++) {
    buf[2*sizeof(key)-i-1] = (key >> (4*i)) & 0xf;
  }
}

template<>
constexpr int DefaultKeyTransformer<uint64_t>::buf_size() {
  return sizeof(uint64_t) * 2;
}
