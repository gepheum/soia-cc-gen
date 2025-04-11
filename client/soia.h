// Soia client library

#ifndef SOIA_SOIA_H_VERSION
#define SOIA_SOIA_H_VERSION 20250403

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "absl/base/nullability.h"
#include "absl/container/flat_hash_map.h"
#include "absl/hash/hash.h"
#include "absl/log/absl_check.h"
#include "absl/log/die_if_null.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "absl/types/optional.h"

namespace soia_internal {
class ByteSink;

template <typename T, typename Getter>
using getter_value_type = std::remove_const_t<
    std::remove_reference_t<decltype(Getter()(std::declval<T&>()))>>;
}  // namespace soia_internal

namespace soia {

// What to do with unrecognized fields when parsing a soia value from JSON or
// binary data.
// Pick kKeep if the input JSON or binary string comes from a trusted program
// which might have been built from more recent source files.
// Always pick kDrop if the input JSON or binary string might come from a
// malicious user.
//
// Default: kDrop
enum class UnrecognizedFieldsPolicy { kDrop, kKeep };

// A string of bytes.
//
// Soia uses std::string exclusively for UTF-8 string, and  ByteString for
// binary data.
class ByteString {
 public:
  ByteString() = default;

  ByteString(absl::string_view str)
      : data_(CopyData(str)), length_(str.length()) {}

  ByteString(std::initializer_list<uint8_t> bytes)
      : ByteString(
            absl::string_view((const char*)bytes.begin(), bytes.size())) {}

  ByteString(const ByteString& other) : ByteString(other.as_string()) {}
  ByteString(ByteString&& other) : data_(other.data_), length_(other.length_) {
    other.data_ = nullptr;
    other.length_ = 0;
  }

  ~ByteString() { FreeData(); }

  ByteString& operator=(absl::string_view other) {
    FreeData();
    data_ = CopyData(other);
    length_ = other.length();
    return *this;
  }
  ByteString& operator=(const ByteString& other) {
    return (*this) = other.as_string();
  }
  ByteString& operator=(ByteString&& other) {
    FreeData();
    data_ = other.data_;
    length_ = other.length_;
    other.data_ = nullptr;
    other.length_ = 0;
    return *this;
  }

  absl::string_view as_string() const& {
    return absl::string_view((const char*)data_, length_);
  }

  std::string as_string() const&& {
    return std::string((const char*)data_, length_);
  }

  size_t length() const { return length_; }

  bool empty() const { return length_ == 0; }

  bool operator==(const ByteString& other) const {
    return as_string() == other.as_string();
  }
  bool operator!=(const ByteString& other) const {
    return as_string() != other.as_string();
  }

 private:
  static_assert(sizeof(char) == 1);

  ByteString(const uint8_t* data, size_t length)
      : data_(data), length_(length) {}

  const uint8_t* data_ = nullptr;
  size_t length_ = 0;

  void FreeData() const {
    if (data_ != nullptr) {
      delete[] data_;
    }
  }

  static uint8_t* CopyData(absl::string_view str) {
    if (str.empty()) return nullptr;
    uint8_t* result = new uint8_t[str.length()];
    std::memcpy(result, (const uint8_t*)str.data(), str.length());
    return result;
  }

  friend class ::soia_internal::ByteSink;
};

template <typename H>
H AbslHashValue(H h, const ByteString& byte_string) {
  return H::combine(std::move(h), byte_string.as_string());
}

template <typename T, typename GetKey>
using key_type =
    std::conditional_t<std::is_same<soia_internal::getter_value_type<T, GetKey>,
                                    std::string>::value,
                       absl::string_view,
                       soia_internal::getter_value_type<T, GetKey>>;

// A vector-like container that stores items of type T and allows for fast
// lookups by key using a hash table. The key is extracted from each item
// using a user-provided GetKey function.
//
// Example:
//   keyed_items<User, soiagen::get_id> users;
//   users.push_back({.id = 1, .name = "Alice"});
//   users.push_back({.id = 2, .name = "Bob"});
//   const User* user = users.find_or_null(1);  // returns pointer to Alice
//
// If multiple items in the container have the same key, only the last one can
// be found when calling find_or_null() or find_or_default().
//
// Accessors, such as at(), operator[], front(), and back() all return constant
// references. Once you have pushed a value into the container, the only way to
// modify the item is to use a vector_mutator, which is obtained by calling
// start_vector_mutation(). The vector_mutator lets you access the underlying
// std::vector<T> and modify it. While the vector mutation is in progress, you
// must not access the keyed_items. When the vector_mutator is destroyed, all
// the items in the vector are rehashed, which runs in O(N).
//
// Example:
//   keyed_items<User, soiagen::get_id> users;
//   users.push_back({.id = 1, .name = "Alice"});
//   users.push_back({.id = 2, .name = "Bob"});
//
//   {
//      auto mutator = users.start_vector_mutation();
//      for (User& user : *mutator) {
//        user.name = absl:::AsciiStrToLower(user.name);
//      }
//   }
//
//   // Okay to access `users` here because the vector_mutator has been
//   // destroyed.
//
//   assert(users.find_or_default(1).name == "alice");
//
// The as_vector() method returns a constant reference to the underlying
// std::vector<T>.
template <typename T, typename GetKey>
class keyed_items {
 public:
  using value_type = T;
  using get_key = GetKey;
  using iterator = typename std::vector<T>::const_iterator;
  using const_iterator = typename std::vector<T>::const_iterator;
  using reverse_iterator = typename std::vector<T>::const_reverse_iterator;
  using const_reverse_iterator =
      typename std::vector<T>::const_reverse_iterator;

  keyed_items() = default;

  keyed_items(const keyed_items& other)
      : vector_(other.vector_),
        slots_bytes_(other.slots_bytes_),
        slot_type_(other.slot_type_) {
    ABSL_CHECK(!other.being_mutated_);
  }

  keyed_items(keyed_items&& other)
      : vector_(std::move(other.vector_)),
        slots_bytes_(std::move(other.slots_bytes_)),
        slot_type_(other.slot_type_) {
    ABSL_CHECK(!other.being_mutated_);
  }

  template <typename InputIt>
  keyed_items(InputIt first, InputIt last) : vector_(first, last) {
    Rehash();
  }

  keyed_items(std::initializer_list<T> init) : vector_(std::move(init)) {
    Rehash();
  }

  keyed_items(std::vector<T> vector) : vector_(std::move(vector)) { Rehash(); }

  ~keyed_items() { ABSL_CHECK(!being_mutated_); }

  keyed_items& operator=(const keyed_items& other) {
    ABSL_CHECK(!being_mutated_);
    ABSL_CHECK(!other.being_mutated_);
    vector_ = other.vector_;
    slots_bytes_ = other.slots_bytes_;
    slot_type_ = other.slot_type_;
    return *this;
  }

  keyed_items& operator=(keyed_items&& other) {
    ABSL_CHECK(!being_mutated_);
    ABSL_CHECK(!other.being_mutated_);
    vector_ = std::move(other.vector_);
    slots_bytes_ = std::move(other.slots_bytes_);
    slot_type_ = std::move(other.slot_type_);
    return *this;
  }

  keyed_items& operator=(std::vector<T> other) {
    ABSL_CHECK(!being_mutated_);
    vector_ = std::move(other);
    Rehash();
    return *this;
  }

  keyed_items& operator=(std::initializer_list<T> init) {
    ABSL_CHECK(!being_mutated_);
    vector_ = std::vector<T>(std::move(init));
    Rehash();
    return *this;
  }

  template <typename InputIt>
  void assign(InputIt first, InputIt last) {
    ABSL_CHECK(!being_mutated_);
    vector_ = std::vector<T>(first, last);
    Rehash();
  }

  const T& at(size_t index) const { return vector().at(index); }
  const T& operator[](size_t index) const { return vector()[index]; }
  const T& front() const { return vector().front(); }
  const T& back() const { return vector().back(); }

  auto begin() const { return vector().begin(); }
  auto cbegin() const { return begin(); }
  auto end() const { return vector().end(); }
  auto cend() const { return end(); }

  auto rbegin() const { return vector().rbegin(); }
  auto crbegin() const { return rbegin(); }
  auto rend() const { return vector().rend(); }
  auto crend() const { return rend(); }

  bool empty() const { return vector_.empty(); }
  size_t size() const { return vector_.size(); }

  void reserve(size_t new_cap) {
    ABSL_CHECK(!being_mutated_);
    vector_.reserve(new_cap);
    MaybeRehash();
  }

  size_t capacity() const { return vector().capacity(); }

  void shrink_to_fit() {
    ABSL_CHECK(!being_mutated_);
    vector_.shrink_to_fit();
    MaybeRehash();
  }

  void clear() {
    ABSL_CHECK(!being_mutated_);
    vector_.clear();
    Rehash();
  }

  void push_back(T value) {
    ABSL_CHECK(!being_mutated_);
    vector_.push_back(std::move(value));
    if (MaybeRehash()) return;
    const size_t next_index = vector_.size();
    using key_type = key_type<T, GetKey>;
    const uint32_t key_hash = absl::Hash<key_type>{}(GetKey()(vector_.back()));
    PutSlot(key_hash, next_index, slot_type_);
  }

  void append_range(const keyed_items<T, GetKey>& rg) { append_range_impl(rg); }
  void append_range(keyed_items<T, GetKey>&& rg) {
    append_range_impl(std::move(rg));
  }
  void append_range(const std::vector<T>& rg) { append_range_impl(rg); }
  void append_range(std::vector<T>&& rg) { append_range_impl(std::move(rg)); }

  void swap(keyed_items& other) noexcept {
    ABSL_CHECK(!being_mutated_);
    ABSL_CHECK(!other.being_mutated_);
    vector_.swap(other.vector_);
    slots_bytes_.swap(other.slots_bytes_);
    std::swap(slot_type_, other.slot_type_);
  }

  // Returns a pointer to the last item with the given key or nullptr if no such
  // item exists.
  template <typename K>
  const T* find_or_null(const K& key) const {
    return find_or_null_impl(key_type<T, GetKey>(key));
  }

  // Returns the last item with the given key or a constant reference to a
  // zero-initialized T if no such item exists.
  //
  // In some cases, this can help you write more succint code than if you were
  // using find_or_null. For example, instead of:
  //
  //   if (users.find_or_null(id) != nullptr &&
  //       !users.find_or_null(id)->name.empty()) { ... }
  //
  // you can write:
  //
  //   if (!users.find_or_default(id).name.empty()) { ... }
  template <typename K>
  const T& find_or_default(const K& key) const {
    return find_or_default_impl(key_type<T, GetKey>(key));
  }

  // Returns the last item with the given key or default_value if no such item
  // exists.
  template <typename K>
  const T& find_or_default(const K& key, const T& default_value) const {
    return find_or_default_impl(key_type<T, GetKey>(key), default_value);
  }

  void sort_by_key() {
    ABSL_CHECK(!being_mutated_);
    std::sort(vector_.begin(), vector_.end(),
              [](const T& a, const T& b) { return GetKey()(a) < GetKey()(b); });
    Rehash();
  }

  const std::vector<T>& vector() const& { return vector_; }
  std::vector<T> vector() && { return std::move(vector_); }

  bool operator==(const keyed_items& other) const {
    return vector() == other.vector();
  }
  bool operator!=(const keyed_items& other) const {
    return vector() != other.vector();
  }

  class vector_mutator {
   public:
    vector_mutator(const vector_mutator&) = delete;
    vector_mutator(vector_mutator&&) = delete;
    vector_mutator& operator=(const vector_mutator&) = delete;
    vector_mutator& operator=(vector_mutator&&) = delete;

    ~vector_mutator() {
      keyed_items_.Rehash();
      keyed_items_.being_mutated_ = false;
    }

    std::vector<T>& operator*() & { return keyed_items_.vector_; }
    std::vector<T>* operator->() & { return &keyed_items_.vector_; }

    std::vector<T>& operator*() && = delete;
    std::vector<T>* operator->() && = delete;

   private:
    vector_mutator(keyed_items* keyed_items) : keyed_items_(*keyed_items) {}

    keyed_items& keyed_items_;

    friend class keyed_items;
  };

  // Starts a vector mutation which will stay active until the vector_mutator
  // is destroyed. While the vector_mutator is active, you must not call any
  // method on the keyed_items.
  // See class-level documentation.
  vector_mutator start_vector_mutation() & {
    ABSL_CHECK(!being_mutated_);
    being_mutated_ = true;
    return vector_mutator(this);
  }

  vector_mutator start_vector_mutation() && = delete;

 private:
  std::vector<T> vector_;
  std::vector<uint8_t> slots_bytes_;
  enum class SlotType {
    kUint8,
    kUint16,
    kUint32,
    kUint64,
  };
  SlotType slot_type_ = SlotType::kUint8;
  bool being_mutated_ = false;

  bool MaybeRehash() {
    const size_t capacity = vector_.capacity();
    const SlotType slot_type = GetSlotType(capacity);
    const size_t num_slots_bytes = GetNumSlotsBytes(capacity, slot_type);
    if (slot_type == slot_type_ && num_slots_bytes == slots_bytes_.size())
      return false;
    Rehash(slot_type, num_slots_bytes);
    return true;
  }

  void Rehash() {
    const size_t capacity = vector_.capacity();
    const SlotType slot_type = GetSlotType(capacity);
    const size_t num_slots_bytes = GetNumSlotsBytes(capacity, slot_type);
    Rehash(slot_type, num_slots_bytes);
  }

  void Rehash(SlotType slot_type, size_t num_slots_bytes) {
    using key_type = key_type<T, GetKey>;
    slot_type_ = slot_type;
    slots_bytes_ = std::vector<uint8_t>(num_slots_bytes);
    for (size_t i = 0; i < vector_.size();) {
      const value_type& item = vector_[i];
      const uint32_t key_hash = absl::Hash<key_type>{}(GetKey()(item));
      ++i;
      PutSlot(key_hash, i, slot_type);
    }
  }

  void PutSlot(uint32_t key_hash, size_t next_index, SlotType slot_type) {
    switch (slot_type) {
      case SlotType::kUint8:
        PutSlot<uint8_t>(key_hash, next_index);
        break;
      case SlotType::kUint16:
        PutSlot<uint16_t>(key_hash, next_index);
        break;
      case SlotType::kUint32:
        PutSlot<uint32_t>(key_hash, next_index);
        break;
      case SlotType::kUint64:
        PutSlot<uint64_t>(key_hash, next_index);
        break;
    }
  }

  template <typename SlotIntType>
  void PutSlot(uint32_t key_hash, size_t next_index) {
    const size_t num_slots = slots_bytes_.size() / sizeof(SlotIntType);
    SlotIntType* slots = (SlotIntType*)slots_bytes_.data();
    size_t slot_index = key_hash % num_slots;
    while (true) {
      SlotIntType& slot = slots[slot_index];
      if (slot == 0) {
        slot = next_index;
        break;
      }
      if (++slot_index == num_slots) {
        slot_index = 0;
      }
    }
  }

  template <typename Range>
  void append_range_impl(const Range& range) {
    reserve(capacity() + range.size());
    for (const T& e : range) {
      push_back(T(e));
    }
  }

  template <typename Range>
  void append_range_impl(Range&& range) {
    reserve(capacity() + range.size());
    for (const T& e : range) {
      push_back(std::move(const_cast<T&>(e)));
    }
  }

  template <typename K>
  const T* find_or_null_impl(K key) const {
    static_assert(std::is_same_v<K, key_type<T, GetKey>>);
    ABSL_CHECK(!being_mutated_);
    if (vector_.empty()) return nullptr;
    const uint32_t key_hash = absl::Hash<K>{}(key);
    switch (slot_type_) {
      case SlotType::kUint8:
        return FindOrNull<uint8_t>(key, key_hash);
      case SlotType::kUint16:
        return FindOrNull<uint16_t>(key, key_hash);
      case SlotType::kUint32:
        return FindOrNull<uint32_t>(key, key_hash);
      default:
        return FindOrNull<uint64_t>(key, key_hash);
    }
  }

  template <typename SlotIntType, typename K>
  const T* FindOrNull(K key, uint32_t key_hash) const {
    static_assert(std::is_same_v<K, key_type<T, GetKey>>);
    const size_t num_slots = slots_bytes_.size() / sizeof(SlotIntType);
    SlotIntType* slots = (SlotIntType*)slots_bytes_.data();
    size_t slot_index = key_hash % num_slots;
    while (true) {
      const SlotIntType slot = slots[slot_index];
      if (slot == 0) return nullptr;
      const T& candidate = vector_[slot - 1];
      if (GetKey()(candidate) == key) return &candidate;
      if (++slot_index == num_slots) {
        slot_index = 0;
      }
    }
  }

  template <typename K>
  const T& find_or_default_impl(K key) const {
    static_assert(std::is_same_v<K, key_type<T, GetKey>>);
    static const T* kDefaultValue = new T{};
    return find_or_default_impl(key, *kDefaultValue);
  }

  template <typename K>
  const T& find_or_default_impl(K key, const T& default_value) const {
    static_assert(std::is_same_v<K, key_type<T, GetKey>>);
    const T* ptr = find_or_null_impl(key);
    return ptr != nullptr ? *ptr : default_value;
  }

  SlotType GetSlotType(size_t capacity) {
    if (capacity <= std::numeric_limits<uint8_t>::max()) {
      return SlotType::kUint8;
    } else if (capacity <= std::numeric_limits<uint16_t>::max()) {
      return SlotType::kUint16;
    } else if (capacity <= std::numeric_limits<uint32_t>::max()) {
      return SlotType::kUint32;
    } else {
      return SlotType::kUint64;
    }
  }

  // Returns the new size of slots_bytes_ for the given vector_'s capactiy.
  static size_t GetNumSlotsBytes(size_t capacity, SlotType slot_type) {
    constexpr int kNumSlotsPerItem = 2;
    switch (slot_type) {
      case SlotType::kUint8:
        return capacity * kNumSlotsPerItem;
      case SlotType::kUint16:
        return capacity * kNumSlotsPerItem * 2;
      case SlotType::kUint32:
        if (std::is_same_v<size_t, uint32_t> &&
            capacity >
                std::numeric_limits<uint32_t>::max() / (kNumSlotsPerItem * 4)) {
          return std::numeric_limits<uint32_t>::max();
        }
        return capacity * kNumSlotsPerItem * 4;
      default:
        return capacity * kNumSlotsPerItem * 8;
    }
  }

  friend class vector_mutator;
};

template <typename H, typename T, typename GetKey>
H AbslHashValue(H h, const keyed_items<T, GetKey>& keyed_items) {
  return H::combine(std::move(h), keyed_items.vector());
}

// A wrapper around a heap-allocated value of type T.
// In its zero-initialized state, the behavior of operator*() depends on whether
// it is called on a const rec<T> or a mutable rec<T>:
//   - On a const rec<T>, operator*() returns a constant reference to a static
//     zero-initialized T
//   - On a mutable rec<T>, operator*() assigns a zero-initialized T to the rec
//     and returns a mutable reference to it. The rec is no longer in a
//     zero-initialized state.
//
// The soia C++ code generator can chose to make the field of a generated struct
// type a rec<T> instead of a T if T is a recursive type.
//
// For example:
//
//   struct Foo {
//     // Foo foo;  // <- does not compile: a struct cannot contain itself
//     rec<Foo> foo;
//     std::string name;
//   };
//
// The word 'rec' stands for 'recursive'.
template <typename T>
class rec {
 public:
  using value_type = T;

  rec() = default;
  rec(const rec& other) {
    if (other.value_ != nullptr) {
      value_ = std::make_unique<T>(*other.value_);
    }
  }
  rec(rec&& other) = default;
  rec(T value) : value_(std::make_unique<T>(std::move(value))) {}

  const T& operator*() const {
    if (value_ != nullptr) {
      return *value_;
    } else {
      static const T* const default_value = new T();
      return *default_value;
    }
  }

  T& operator*() {
    if (value_ != nullptr) {
      return *value_;
    } else {
      return *(value_ = std::make_unique<T>());
    }
  }

  operator const T&() const { return **this; }
  operator T&() { return **this; }

  const T* operator->() const { return &(**this); }
  T* operator->() { return &(**this); }

  rec& operator=(const rec& other) {
    if (other.value_ != nullptr) {
      value_ = std::make_unique<T>(*other.value_);
    } else {
      value_ = nullptr;
    }
    return *this;
  }

  rec& operator=(rec&& other) = default;

  rec& operator=(T other) {
    value_ = std::make_unique<T>(std::move(other));
    return *this;
  }

  bool operator==(const rec& other) const { return **this == *other; }
  bool operator!=(const rec& other) const { return **this != *other; }

 private:
  std::unique_ptr<T> value_;
};

template <typename T>
rec<T> make_rec(T value) {
  return rec<T>(std::move(value));
}

template <typename H, typename T>
H AbslHashValue(H h, const rec<T>& rec) {
  return H::combine(std::move(h), *rec);
}

template <typename O, typename T>
std::ostream& operator<<(std::ostream& os, const rec<T>& input) {
  return os << *input;
}

template <typename T>
class must_init {
 public:
  using value_type = T;

  template <typename Value>
  must_init(Value value) {
    value_ = std::move(value);
  }

  T operator*() && { return std::move(value_); }

 private:
  T value_;
};

template <typename T>
class must_init<std::vector<T>> {
 public:
  using value_type = std::vector<T>;
  must_init(value_type value) { value_ = std::move(value); }
  must_init(std::initializer_list<T> l) : must_init(value_type(std::move(l))) {}
  value_type operator*() && { return std::move(value_); }

 private:
  value_type value_;
};

template <typename T, typename GetKey>
class must_init<soia::keyed_items<T, GetKey>> {
 public:
  using value_type = soia::keyed_items<T, GetKey>;
  must_init(value_type value) { value_ = std::move(value); }
  must_init(std::initializer_list<T> l) : must_init(value_type(std::move(l))) {}
  value_type operator*() && { return std::move(value_); }

 private:
  value_type value_;
};

struct identity {
  template <typename T>
  T&& operator()(T&& t) const {
    return std::forward<T>(t);
  }
};

template <typename other = identity>
struct get_kind {
  using other_type = other;

  static constexpr absl::string_view kFieldName = "kind";

  template <typename E>
  auto operator()(const E& e) const {
    return other()(e).kind();
  }
};

namespace reflection {

enum class PrimitiveType {
  kBool,
  kInt32,
  kInt64,
  kUint64,
  kFloat32,
  kFloat64,
  kTimestamp,
  kString,
  kBytes,
};

struct OptionalType;
struct ArrayType;
struct RecordType;

using Type = std::variant<PrimitiveType, OptionalType, ArrayType, RecordType>;

struct OptionalType {
  soia::rec<Type> other;
};

struct ArrayType {
  soia::rec<Type> item;
  std::vector<std::string> key_chain;
};

struct RecordType {
  std::string record_id;
};

enum class RecordKind {
  kStruct,
  kEnum,
};

struct Field {
  std::string name;
  // Can only be nullopt if the record is an enum and the field is a constant
  // field.
  absl::optional<Type> type;
  int number{};
};

struct get_name {
  const std::string& operator()(const Field& field) { return field.name; }
};

// Definition of a soia struct or enum.
struct Record {
  RecordKind kind{};
  std::string id;
  keyed_items<Field, get_name> fields;
  std::vector<int> removed_fields;
};

struct get_id {
  const std::string& operator()(const Record& record) const {
    return record.id;
  }
};

// Describes a soia type. Contains the definition of all the structs and enums
// referenced from the type.
//
// A TypeDescriptor can be serialized to JSON with AsJson(), and deserialized
// from JSON with FromJson().
struct TypeDescriptor {
  Type type;
  keyed_items<Record, get_id> records;

  std::string AsJson() const;

  static absl::StatusOr<TypeDescriptor> FromJson(absl::string_view json);
};

using RecordRegistry = keyed_items<Record, get_id>;

// For static reflection: represents a field of a soia struct.
// See `soia::reflection::ForEachField`
template <typename Getter, typename Value>
struct struct_field {
  using getter_type = Getter;
  using value_type = Value;
};

// For static reflection: represents a constant field of a soia enum.
// See `soia::reflection::ForEachField`
template <typename Const>
struct enum_const_field {
  using const_type = Const;
};

// For static reflection: represents a value field of a soia enum.
// See `soia::reflection::ForEachField`
template <typename Option, typename Value>
struct enum_value_field {
  using option_type = Option;
  using value_type = Value;
};

}  // namespace reflection

namespace service {

enum class ResponseType {
  // The method invocation succeeded and the response data is in JSON format.
  kOkJson,
  // The method invocation failed because the request was malformed.
  // The response data is "bad-request:" followed by an error message.
  kBadRequest,
  // The method invocation resulted in an error on the server side.
  // The response data is "server-error:" followed by an error message.
  kServerError,
};

// Raw response returned by the server.
struct RawResponse {
  // The meaning of this string depends on the response type.
  std::string data;
  ResponseType type{};

  int status_code() const {
    switch (type) {
      case ResponseType::kOkJson:
        return 200;
      case ResponseType::kBadRequest:
        return 400;
      case ResponseType::kServerError:
        return 500;
    }
  }

  absl::string_view content_type() const {
    switch (type) {
      case ResponseType::kOkJson: {
        static const char kApplicationJson[] = "application/json";
        return kApplicationJson;
      }
      case ResponseType::kBadRequest:
      case ResponseType::kServerError: {
        static const char kTextPlain[] = "text/plain";
        return kTextPlain;
      }
    }
  }

  absl::StatusOr<std::string> AsStatus() && {
    switch (type) {
      case ResponseType::kOkJson:
        return std::move(data);
      case ResponseType::kBadRequest:
      case ResponseType::kServerError:
        return absl::UnknownError(std::move(data));
    }
  }
};

class HttpHeaders {
 public:
  HttpHeaders() = default;
  HttpHeaders(const HttpHeaders&) = default;
  HttpHeaders(HttpHeaders&&) = default;

  HttpHeaders& operator=(const HttpHeaders&) = default;
  HttpHeaders& operator=(HttpHeaders&&) = default;

  // Appends `value` to the vector of values associated with the given name
  // converted to lowercase.
  void Insert(absl::string_view name, absl::string_view value);

  // Returns the vector of values associated with the given name converted to
  // lowercase. If there is no value associated with the given name, returns an
  // empty vector.
  const std::vector<std::string>& Get(absl::string_view name) const;

  // Returns the last value associated with the given name converted to
  // lowercase, or an empty string if there is none.
  absl::string_view GetLast(absl::string_view name) const;

  // Returns a map view of this HttpHeaders object.
  const absl::flat_hash_map<std::string, std::vector<std::string>>& map()
      const {
    return map_;
  }

 private:
  absl::flat_hash_map<std::string, std::vector<std::string>> map_;
};

// Sends RPCs to a soia service.
class Client {
 public:
  virtual ~Client() = default;

  virtual absl::StatusOr<std::string> operator()(
      absl::string_view request_data, const HttpHeaders& request_headers,
      HttpHeaders& response_headers) const = 0;
};

}  // namespace service
}  // namespace soia

namespace soia_internal {

template <typename T>
T& get(T& input) {
  return input;
}
template <typename T>
T& get(soia::rec<T>& input) {
  return *input;
}
template <typename T>
const T& get(const soia::rec<T>& input) {
  return *input;
}

template <typename Struct, typename Getter>
using struct_field =
    soia::reflection::struct_field<Getter, getter_value_type<Struct, Getter>>;

template <typename Enum, typename Option>
using enum_value_field = soia::reflection::enum_value_field<
    Option, std::remove_const_t<std::remove_pointer_t<
                decltype(Option::get_or_null(std::declval<Enum&>()))>>>;

struct NewLine {
 public:
  const std::string& Indent() {
    new_line_ += {' ', ' '};
    return new_line_;
  }

  const std::string& Dedent() {
    new_line_.resize(new_line_.length() - 2);
    return new_line_;
  }

  const std::string& operator*() const { return new_line_; }

 private:
  std::string new_line_ = "\n";
};

struct DenseJson {
  std::string out;
};

struct ReadableJson {
  std::string out;
  NewLine new_line;
};

struct DebugString {
  std::string out;
  NewLine new_line;
};

class ByteSink {
 public:
  ByteSink() = default;
  ByteSink(const ByteSink&) = delete;
  ByteSink(ByteSink&&) = delete;

  ~ByteSink() {
    if (data_ != nullptr) {
      delete[] data_;
    }
  }

  ByteSink& operator=(const ByteSink& other) = delete;
  ByteSink& operator=(ByteSink&& other) = delete;

  size_t length() const { return pos_ - data_; }

  // Invalidated by anything that can change the capacity of the byte sink.
  uint8_t* data() { return data_; }
  // Invalidated by anything that can change the capacity of the byte sink.
  const uint8_t* data() const { return data_; }

  // Invalidated by anything that can change the capacity of the byte sink.
  uint8_t* pos() { return pos_; }
  // The given pointer must be in the [data, data + length] range.
  void set_pos(uint8_t* pos) { pos_ = pos; }

  // Prepares for N bytes to be pushed with PushUnsafe().
  void Prepare(size_t n) {
    if (n <= capacity_left()) return;
    const size_t len = length();
    capacity_ += std::max(capacity_, n);
    uint8_t* new_data = new uint8_t[capacity_];
    std::memcpy(new_data, data_, len);
    delete[] data_;
    data_ = new_data;
    pos_ = data_ + len;
  }

  void PushUnsafe(uint8_t byte) { *(pos_++) = byte; }

  template <typename Byte, typename... Bytes>
  void PushUnsafe(uint8_t first, Byte second, Bytes... tail) {
    PushUnsafe(first);
    PushUnsafe(second, tail...);
  }

  template <typename... Bytes>
  void Push(uint8_t first, Bytes... tail) {
    Prepare(1 + sizeof...(tail));
    PushUnsafe(first, tail...);
  }

  void PushNUnsafe(const uint8_t* src, size_t n) {
    std::memcpy((pos_ += n) - n, src, n);
  }

  void PushN(const uint8_t* src, size_t n) {
    Prepare(n);
    PushNUnsafe(src, n);
  }

  void PushRangeUnsafe(const uint8_t* begin, const uint8_t* end) {
    PushNUnsafe(begin, end - begin);
  }

  void PushRange(const uint8_t* begin, const uint8_t* end) {
    PushN(begin, end - begin);
  }

  void PopN(size_t n) { pos_ -= n; }

  ::soia::ByteString ToByteString() && {
    ::soia::ByteString byte_string(data_, length());
    // To prevent the ByteString destructor from deleting the array.
    data_ = nullptr;
    return byte_string;
  }

 private:
  static constexpr size_t kDefaultCapacity = 16;
  size_t capacity_ = kDefaultCapacity;
  uint8_t* data_ = new uint8_t[kDefaultCapacity];
  uint8_t* pos_ = data_;

  size_t capacity_left() const { return capacity_ - length(); }
};

struct ByteSource {
  ByteSource(const uint8_t* begin, size_t length)
      : pos(begin), end(begin + length) {}
  ByteSource(const char* begin, size_t length)
      : ByteSource((const uint8_t*)begin, length) {}

  const uint8_t* pos = nullptr;
  const uint8_t* const end;
  bool keep_unrecognized_fields = false;
  bool error = false;

  size_t num_bytes_left() const { return end - pos; }

  uint8_t ReadWire() {
    if (pos < end) {
      return *(pos++);
    } else {
      RaiseError();
      return 0;
    }
  }

  // Same as ReadWire, but does not increment pos.
  uint8_t PeekWire() {
    if (pos < end) {
      return *pos;
    } else {
      RaiseError();
      return 0;
    }
  }

  bool TryAdvance(size_t n) {
    if ((pos += n) <= end) return true;
    RaiseError();
    return false;
  }

  void CheckEnd() {
    if (pos > end) {
      RaiseError();
    }
  }

  void RaiseError() {
    pos = end;
    error = true;
  }
};

enum class JsonTokenType {
  kError = '!',
  kTrue = 't',
  kFalse = 'f',
  kNull = 'n',
  kZero = '0',
  kUnsignedInteger = '+',
  kSignedInteger = '-',
  kFloat = '.',
  kString = '"',
  kLeftSquareBracket = '[',
  kRightSquareBracket = ']',
  kLeftCurlyBracket = '{',
  kRightCurlyBracket = '}',
  kComma = ',',
  kColon = ':',
  kStrEnd = '\0',
};

class JsonTokenizer {
 public:
  JsonTokenizer(const char* json_code_begin, const char* json_code_end,
                soia::UnrecognizedFieldsPolicy unrecognized_fields)
      : keep_unrecognized_fields_(unrecognized_fields ==
                                  soia::UnrecognizedFieldsPolicy::kKeep) {
    state_.begin = json_code_begin;
    state_.pos = json_code_begin;
    state_.end = json_code_end;
  }

  JsonTokenizer(const JsonTokenizer&) = delete;
  JsonTokenizer(JsonTokenizer&&) = delete;
  JsonTokenizer& operator=(const JsonTokenizer&) = delete;
  JsonTokenizer& operator=(JsonTokenizer&&) = delete;

  JsonTokenType Next();

  struct State {
    const char* begin = nullptr;
    const char* pos = nullptr;
    const char* end = nullptr;
    absl::Status status = absl::OkStatus();
    JsonTokenType token_type = JsonTokenType::kStrEnd;
    int64_t int_value = 0;
    uint64_t uint_value = 0;
    double float_value = 0.0;
    std::string string_value;

    size_t chars_left() const { return end - pos; }

    char NextCharOrNull() { return ++pos < end ? *pos : '\0'; }

    void PushError(absl::string_view message);
    void PushErrorAtPosition(absl::string_view expected);
    void PushUnexpectedTokenError(absl::string_view expected);
  };

  State& mutable_state() { return state_; }
  const State& state() const { return state_; }

  const bool keep_unrecognized_fields() const {
    return keep_unrecognized_fields_;
  }

 private:
  const bool keep_unrecognized_fields_;
  State state_;
};

void SkipValue(JsonTokenizer& tokenizer);

class JsonArrayCloser {
 public:
  explicit JsonArrayCloser(DenseJson* json) : json_(json->out) {}
  ~JsonArrayCloser() { json_ += ']'; }

 private:
  std::string& json_;
};

class JsonArrayReader {
 public:
  explicit JsonArrayReader(JsonTokenizer* tokenizer);
  bool NextElement();
  JsonTokenizer& tokenizer() { return tokenizer_; }

 private:
  JsonTokenizer& tokenizer_;
  bool zero_state_ = true;
};

class JsonObjectReader {
 public:
  explicit JsonObjectReader(JsonTokenizer* tokenizer);

  bool NextEntry();

  // Name component of the current entry.
  // Invalidated when NextEntry() is called.
  const std::string& name() const { return name_; }

 private:
  JsonTokenizer& tokenizer_;
  std::string name_;
  bool zero_state_ = true;
};

void SkipValue(ByteSource& source);

void AppendArrayPrefix(size_t length, ByteSink& out);

void ParseArrayPrefix(ByteSource& source, uint32_t& length);

template <typename T>
struct soia_type {};

struct BoolAdapter {
  static bool IsDefault(bool input) { return !input; }

  static void Append(bool input, DenseJson& out) {
    out.out += input ? '1' : '0';
  }

  static void Append(bool input, ReadableJson& out) {
    AppendTrueOrFalse(input, out.out);
  }

  static void Append(bool input, DebugString& out) {
    AppendTrueOrFalse(input, out.out);
  }

  static void Append(bool input, ByteSink& out) { out.Push(input ? 1 : 0); }

  static void Parse(JsonTokenizer& tokenizer, bool& out);

  static void Parse(ByteSource& source, bool& out);

  static soia::reflection::Type GetType(soia_type<bool>) {
    return soia::reflection::PrimitiveType::kBool;
  }

  static void RegisterRecords(soia_type<bool>,
                              soia::reflection::RecordRegistry&) {}

  static constexpr bool IsStruct() { return false; }
  static constexpr bool IsEnum() { return false; }

 private:
  static void AppendTrueOrFalse(bool input, std::string& out) {
    if (input) {
      out += {'t', 'r', 'u', 'e'};
    } else {
      out += {'f', 'a', 'l', 's', 'e'};
    }
  }
};

struct Int32Adapter {
  static bool IsDefault(int32_t input) { return !input; }

  template <typename Out>
  static void Append(int32_t input, Out& out) {
    absl::StrAppend(&out.out, input);
  }

  static void Append(int32_t input, ByteSink& out);

  static void Parse(JsonTokenizer& tokenizer, int32_t& out);
  static void Parse(ByteSource& source, int32_t& out);

  static soia::reflection::Type GetType(soia_type<int32_t>) {
    return soia::reflection::PrimitiveType::kInt32;
  }

  static void RegisterRecords(soia_type<int32_t>,
                              soia::reflection::RecordRegistry&) {}

  static constexpr bool IsStruct() { return false; }
  static constexpr bool IsEnum() { return false; }
};

constexpr int64_t kMinSafeJavascriptInt = -9007199254740992;  // -(2 ^ 53)
constexpr int64_t kMaxSafeJavascriptInt = 9007199254740992;   // 2 ^ 53

struct Int64Adapter {
  static bool IsDefault(int64_t input) { return !input; }

  template <typename Out>
  static void Append(int64_t input, Out& out) {
    if (soia_internal::kMinSafeJavascriptInt <= input &&
        input <= soia_internal::kMaxSafeJavascriptInt) {
      absl::StrAppend(&out.out, input);
    } else {
      out.out += '"';
      absl::StrAppend(&out.out, input);
      out.out += '"';
    }
  }

  static void Append(int64_t input, DebugString& out) {
    absl::StrAppend(&out.out, input);
  }

  static void Append(int64_t input, ByteSink& out);
  static void Parse(JsonTokenizer& tokenizer, int64_t& out);
  static void Parse(ByteSource& source, int64_t& out);

  static soia::reflection::Type GetType(soia_type<int64_t>) {
    return soia::reflection::PrimitiveType::kInt64;
  }

  static void RegisterRecords(soia_type<int64_t>,
                              soia::reflection::RecordRegistry&) {}

  static constexpr bool IsStruct() { return false; }
  static constexpr bool IsEnum() { return false; }
};

struct Uint64Adapter {
  static bool IsDefault(uint64_t input) { return !input; }

  template <typename Out>
  static void Append(uint64_t input, Out& out) {
    if (input <= soia_internal::kMaxSafeJavascriptInt) {
      absl::StrAppend(&out.out, input);
    } else {
      out.out += '"';
      absl::StrAppend(&out.out, input);
      out.out += '"';
    }
  }

  static void Append(int64_t input, DebugString& out) {
    absl::StrAppend(&out.out, input);
  }

  static void Append(uint64_t input, ByteSink& out);
  static void Parse(JsonTokenizer& tokenizer, uint64_t& out);
  static void Parse(ByteSource& source, uint64_t& out);

  static soia::reflection::Type GetType(soia_type<uint64_t>) {
    return soia::reflection::PrimitiveType::kUint64;
  }

  static void RegisterRecords(soia_type<uint64_t>,
                              soia::reflection::RecordRegistry&) {}

  static constexpr bool IsStruct() { return false; }
  static constexpr bool IsEnum() { return false; }
};

template <typename float_or_double>
void AppendJsonForFloat(float_or_double input, std::string& out) {
  if (std::isfinite(input)) {
    absl::StrAppend(&out, input);
  } else if (std::isinf(input)) {
    if (input < 0) {
      out += {'"', '-', 'I', 'n', 'f', 'i', 'n', 'i', 't', 'y', '"'};
    } else {
      out += {'"', 'I', 'n', 'f', 'i', 'n', 'i', 't', 'y', '"'};
    }
  } else {
    out += {'"', 'N', 'a', 'N', '"'};
  }
}

struct Float32Adapter {
  static bool IsDefault(float input) { return !input; }

  template <typename Out>
  static void Append(float input, Out& out) {
    AppendJsonForFloat(input, out.out);
  }

  static void Append(float input, DebugString& out) {
    if (std::isfinite(input)) {
      absl::StrAppend(&out.out, input);
    } else if (std::isinf(input)) {
      if (input < 0) {
        out.out += "-std::numeric_limits<float>::infinity()";
      } else {
        out.out += "std::numeric_limits<float>::infinity()";
      }
    } else {
      out.out += "std::numeric_limits<float>::quiet_NaN()";
    }
  }

  static void Append(float input, ByteSink& out);
  static void Parse(JsonTokenizer& tokenizer, float& out);
  static void Parse(ByteSource& source, float& out);

  static soia::reflection::Type GetType(soia_type<float>) {
    return soia::reflection::PrimitiveType::kFloat32;
  }

  static void RegisterRecords(soia_type<float>,
                              soia::reflection::RecordRegistry&) {}

  static constexpr bool IsStruct() { return false; }
  static constexpr bool IsEnum() { return false; }
};

struct Float64Adapter {
  static bool IsDefault(double input) { return !input; }

  template <typename Out>
  static void Append(double input, Out& out) {
    AppendJsonForFloat(input, out.out);
  }

  static void Append(double input, DebugString& out) {
    if (std::isfinite(input)) {
      absl::StrAppend(&out.out, input);
    } else if (std::isinf(input)) {
      if (input < 0) {
        out.out += "-std::numeric_limits<double>::infinity()";
      } else {
        out.out += "std::numeric_limits<double>::infinity()";
      }
    } else {
      out.out += "std::numeric_limits<double>::quiet_NaN()";
    }
  }

  static void Append(double input, ByteSink& out);
  static void Parse(JsonTokenizer& tokenizer, double& out);
  static void Parse(ByteSource& source, double& out);

  static soia::reflection::Type GetType(soia_type<double>) {
    return soia::reflection::PrimitiveType::kFloat64;
  }

  static void RegisterRecords(soia_type<double>,
                              soia::reflection::RecordRegistry&) {}

  static constexpr bool IsStruct() { return false; }
  static constexpr bool IsEnum() { return false; }
};

struct TimestampAdapter {
  static bool IsDefault(absl::Time input) { return !absl::ToUnixMillis(input); }

  static void Append(absl::Time input, DenseJson& out);
  static void Append(absl::Time input, ReadableJson& out);
  static void Append(absl::Time input, DebugString& out);
  static void Append(absl::Time input, ByteSink& out);
  static void Parse(JsonTokenizer& tokenizer, absl::Time& out);
  static void Parse(ByteSource& source, absl::Time& out);

  static soia::reflection::Type GetType(soia_type<absl::Time>) {
    return soia::reflection::PrimitiveType::kTimestamp;
  }

  static void RegisterRecords(soia_type<absl::Time>,
                              soia::reflection::RecordRegistry&) {}

  static constexpr bool IsStruct() { return false; }
  static constexpr bool IsEnum() { return false; }
};

struct StringAdapter {
  static bool IsDefault(const std::string& input) { return input.empty(); }

  template <typename Out>
  static void Append(const std::string& input, Out& out) {
    AppendJson(input, out.out);
  }

  static void AppendJson(const std::string& input, std::string& out);
  static void Append(const std::string& input, DebugString& out);
  static void Append(const std::string& input, ByteSink& out);
  static void Parse(JsonTokenizer& tokenizer, std::string& out);
  static void Parse(ByteSource& source, std::string& out);

  static soia::reflection::Type GetType(soia_type<std::string>) {
    return soia::reflection::PrimitiveType::kString;
  }

  static void RegisterRecords(soia_type<std::string>,
                              soia::reflection::RecordRegistry&) {}

  static constexpr bool IsStruct() { return false; }
  static constexpr bool IsEnum() { return false; }
};

struct BytesAdapter {
  static bool IsDefault(const soia::ByteString& input) { return input.empty(); }

  template <typename Out>
  static void Append(const soia::ByteString& input, Out& out) {
    AppendJson(input, out.out);
  }

  static void AppendJson(const soia::ByteString&, std::string& out);

  static void Append(const soia::ByteString& input, DebugString& out);
  static void Append(const soia::ByteString& input, ByteSink& out);
  static void Parse(JsonTokenizer& tokenizer, soia::ByteString& out);
  static void Parse(ByteSource& source, soia::ByteString& out);

  static soia::reflection::Type GetType(soia_type<soia::ByteString>) {
    return soia::reflection::PrimitiveType::kBytes;
  }

  static void RegisterRecords(soia_type<soia::ByteString>,
                              soia::reflection::RecordRegistry&) {}

  static constexpr bool IsStruct() { return false; }
  static constexpr bool IsEnum() { return false; }
};

template <typename T>
void GetAdapter(soia_type<T>) {
  static_assert(false, "not a soia type");
}

inline BoolAdapter GetAdapter(soia_type<bool>);
inline Int32Adapter GetAdapter(soia_type<int32_t>);
inline Int64Adapter GetAdapter(soia_type<int64_t>);
inline Uint64Adapter GetAdapter(soia_type<uint64_t>);
inline Float32Adapter GetAdapter(soia_type<float>);
inline Float64Adapter GetAdapter(soia_type<double>);
inline TimestampAdapter GetAdapter(soia_type<absl::Time>);
inline StringAdapter GetAdapter(soia_type<std::string>);
inline BytesAdapter GetAdapter(soia_type<soia::ByteString>);

// =============================================================================
// BEGIN serialization of type descriptors
// =============================================================================

struct ReflectionTypeAdapter {
  static bool IsDefault(const soia::reflection::Type& input) { return false; }
  static void Append(const soia::reflection::Type& input, ReadableJson& out);
  static void Parse(JsonTokenizer& tokenizer, soia::reflection::Type& out);
};

struct ReflectionPrimitiveTypeAdapter {
  static bool IsDefault(soia::reflection::PrimitiveType input) { return false; }
  static void Append(soia::reflection::PrimitiveType input, ReadableJson& out);
  static void Parse(JsonTokenizer& tokenizer,
                    soia::reflection::PrimitiveType& out);
};

struct ReflectionOptionalTypeAdapter {
  static bool IsDefault(const soia::reflection::OptionalType& input) {
    return false;
  }
  static void Append(const soia::reflection::OptionalType& input,
                     ReadableJson& out);
  static void Parse(JsonTokenizer& tokenizer,
                    soia::reflection::OptionalType& out);
};

struct ReflectionArrayTypeAdapter {
  static bool IsDefault(const soia::reflection::ArrayType& input) {
    return false;
  }
  static void Append(const soia::reflection::ArrayType& input,
                     ReadableJson& out);
  static void Parse(JsonTokenizer& tokenizer, soia::reflection::ArrayType& out);
};

struct ReflectionRecordTypeAdapter {
  static bool IsDefault(const soia::reflection::RecordType& input) {
    return false;
  }
  static void Append(const soia::reflection::RecordType& input,
                     ReadableJson& out);
  static void Parse(JsonTokenizer& tokenizer,
                    soia::reflection::RecordType& out);
};

struct ReflectionRecordKindAdapter {
  static bool IsDefault(const soia::reflection::RecordKind& input) {
    return false;
  }
  static void Append(const soia::reflection::RecordKind& input,
                     ReadableJson& out);
  static void Parse(JsonTokenizer& tokenizer,
                    soia::reflection::RecordKind& out);
};

struct ReflectionFieldAdapter {
  static bool IsDefault(const soia::reflection::Field& input) { return false; }
  static void Append(const soia::reflection::Field& input, ReadableJson& out);
  static void Parse(JsonTokenizer& tokenizer, soia::reflection::Field& out);
};

struct ReflectionRecordAdapter {
  static bool IsDefault(const soia::reflection::Record& input) { return false; }
  static void Append(const soia::reflection::Record& input, ReadableJson& out);
  static void Parse(JsonTokenizer& tokenizer, soia::reflection::Record& out);
};

struct ReflectionTypeDescriptorAdapter {
  static bool IsDefault(const soia::reflection::TypeDescriptor& input) {
    return false;
  }
  static void Append(const soia::reflection::TypeDescriptor& input,
                     ReadableJson& out);
  static void Parse(JsonTokenizer& tokenizer,
                    soia::reflection::TypeDescriptor& out);
};

inline ReflectionTypeAdapter GetAdapter(soia_type<soia::reflection::Type>);
inline ReflectionPrimitiveTypeAdapter GetAdapter(
    soia_type<soia::reflection::PrimitiveType>);
inline ReflectionOptionalTypeAdapter GetAdapter(
    soia_type<soia::reflection::OptionalType>);
inline ReflectionArrayTypeAdapter GetAdapter(
    soia_type<soia::reflection::ArrayType>);
inline ReflectionRecordKindAdapter GetAdapter(
    soia_type<soia::reflection::RecordKind>);
inline ReflectionRecordTypeAdapter GetAdapter(
    soia_type<soia::reflection::RecordType>);
inline ReflectionFieldAdapter GetAdapter(soia_type<soia::reflection::Field>);
inline ReflectionRecordAdapter GetAdapter(soia_type<soia::reflection::Record>);
inline ReflectionTypeDescriptorAdapter GetAdapter(
    soia_type<soia::reflection::TypeDescriptor>);

// =============================================================================
// END serialization of type descriptors
// =============================================================================

template <typename T>
using TypeAdapter = decltype(GetAdapter(soia_type<T>()));

template <typename T>
bool IsDefault(const T& input) {
  return TypeAdapter<T>::IsDefault(input);
}
template <typename T, typename Out>
void Append(const T& input, Out& out) {
  TypeAdapter<T>::Append(input, out);
}
template <typename Source, typename T>
void Parse(Source& source, T& out) {
  TypeAdapter<T>::Parse(source, out);
}
template <typename T>
soia::reflection::Type GetType() {
  return TypeAdapter<T>::GetType(soia_type<T>());
}

template <typename T>
void RegisterRecords(soia::reflection::RecordRegistry& registry) {
  TypeAdapter<T>::RegisterRecords(soia_type<T>(), registry);
}

template <typename T>
std::string ToDebugString(const T& input) {
  static_assert(!std::is_pointer<T>::value,
                "Can't pass a pointer to ToDebugString");
  soia_internal::DebugString result;
  Append(input, result);
  return result.out;
}

inline std::string ToDebugString(const char* input) {
  return ToDebugString(std::string(input));
}

template <typename T>
std::false_type is_optional(T) {
  return std::false_type();
}
template <typename T>
std::true_type is_optional(absl::optional<T>) {
  return std::true_type();
}

struct OptionalAdapter {
  template <typename T>
  static bool IsDefault(const absl::optional<T>& input) {
    static_assert(
        std::is_same<decltype(is_optional(T())), std::false_type>::value);
    return !input.has_value();
  }

  template <typename T, typename Out>
  static void Append(const absl::optional<T>& input, Out& out) {
    if (input.has_value()) {
      TypeAdapter<T>::Append(*input, out);
    } else {
      out.out += {'n', 'u', 'l', 'l'};
    }
  }

  template <typename T>
  static void Append(const absl::optional<T>& input, DebugString& out) {
    if (input.has_value()) {
      out.out += "absl::make_optional(";
      TypeAdapter<T>::Append(*input, out);
      out.out += ')';
    } else {
      out.out += "absl::nullopt";
    }
  }

  template <typename T>
  static void Append(const absl::optional<T>& input, ByteSink& out) {
    if (input.has_value()) {
      TypeAdapter<T>::Append(*input, out);
    } else {
      out.Push(255);
    }
  }

  template <typename T>
  static void Parse(JsonTokenizer& tokenizer, absl::optional<T>& out) {
    if (tokenizer.state().token_type == JsonTokenType::kNull) {
      tokenizer.Next();
    } else {
      soia_internal::Parse(tokenizer, out.emplace());
    }
  }

  template <typename T>
  static void Parse(ByteSource& source, absl::optional<T>& out) {
    if (source.PeekWire() == 255) {
      ++(source.pos);
    } else {
      TypeAdapter<T>::Parse(source, out.emplace());
    }
  }

  template <typename T>
  static soia::reflection::Type GetType(soia_type<absl::optional<T>>) {
    return soia::reflection::OptionalType{soia_internal::GetType<T>()};
  }

  template <typename T>
  static void RegisterRecords(soia_type<absl::optional<T>>,
                              soia::reflection::RecordRegistry& registry) {
    soia_internal::RegisterRecords<T>(registry);
  }

  static constexpr bool IsStruct() { return false; }
  static constexpr bool IsEnum() { return false; }
};

template <typename T>
inline OptionalAdapter GetAdapter(soia_type<absl::optional<T>>);

template <typename GetKey>
void MakeKeyChain(std::vector<std::string>& out) {
  using other_type = typename GetKey::other_type;
  MakeKeyChain<other_type>(out);
  out.emplace_back(GetKey::kFieldName);
}
template <>
inline void MakeKeyChain<soia::identity>(std::vector<std::string>& out) {}

struct ArrayAdapter {
  template <typename Input>
  static bool IsDefault(const Input& input) {
    return input.empty();
  }

  template <typename Input>
  static void Append(const Input& input, DenseJson& out) {
    using T = typename Input::value_type;
    if (input.empty()) {
      out.out += {'[', ']'};
    } else {
      out.out += '[';
      TypeAdapter<T>::Append(input[0], out);
      for (size_t i = 1; i < input.size(); ++i) {
        out.out += ',';
        TypeAdapter<T>::Append(input[i], out);
      }
      out.out += ']';
    }
  }

  template <typename Input>
  static void Append(const Input& input, ReadableJson& out) {
    using T = typename Input::value_type;
    if (input.empty()) {
      out.out += {'[', ']'};
    } else {
      out.out += '[';
      out.out += out.new_line.Indent();
      TypeAdapter<T>::Append(input[0], out);
      for (size_t i = 1; i < input.size(); ++i) {
        out.out += ',';
        out.out += *out.new_line;
        TypeAdapter<T>::Append(input[i], out);
      }
      out.out += out.new_line.Dedent();
      out.out += ']';
    }
  }

  template <typename Input>
  static void Append(const Input& input, DebugString& out) {
    using T = typename Input::value_type;
    if (input.empty()) {
      out.out += {'{', '}'};
    } else {
      out.out += '{';
      out.new_line.Indent();
      for (const T& element : input) {
        out.out += *out.new_line;
        TypeAdapter<T>::Append(element, out);
        out.out += ',';
      }
      out.out += out.new_line.Dedent();
      out.out += '}';
    }
  }

  template <typename Input>
  static void Append(const Input& input, ByteSink& out) {
    using T = typename Input::value_type;
    AppendArrayPrefix(input.size(), out);
    for (const T& item : input) {
      TypeAdapter<T>::Append(item, out);
    }
  }

  template <typename Out>
  static void Parse(JsonTokenizer& tokenizer, Out& out) {
    switch (tokenizer.state().token_type) {
      case JsonTokenType::kLeftSquareBracket:
        break;
      case JsonTokenType::kZero:
        tokenizer.Next();
        return;
      default:
        tokenizer.mutable_state().PushErrorAtPosition("'['");
    }
    JsonArrayReader array_reader(&tokenizer);
    while (array_reader.NextElement()) {
      ParseAndPush(tokenizer, out);
    }
  }

  template <typename Out>
  static void Parse(ByteSource& source, Out& out) {
    uint32_t length = 0;
    ParseArrayPrefix(source, length);
    if (source.num_bytes_left() < length) {
      return source.RaiseError();
    };
    out.reserve(length);
    for (size_t i = 0; i < length; ++i) {
      if (source.error) return;
      ParseAndPush(source, out);
    }
  }

  template <typename T>
  static soia::reflection::Type GetType(soia_type<std::vector<T>>) {
    return soia::reflection::ArrayType{soia_internal::GetType<T>()};
  }

  template <typename T, typename GetKey>
  static soia::reflection::Type GetType(
      soia_type<soia::keyed_items<T, GetKey>>) {
    std::vector<std::string> key_chain;
    MakeKeyChain<GetKey>(key_chain);
    return soia::reflection::ArrayType{soia_internal::GetType<T>(),
                                       std::move(key_chain)};
  }

  template <typename T>
  static void RegisterRecords(soia_type<T>,
                              soia::reflection::RecordRegistry& registry) {
    soia_internal::RegisterRecords<typename T::value_type>(registry);
  }

  static constexpr bool IsStruct() { return false; }
  static constexpr bool IsEnum() { return false; }

 private:
  template <typename Source>
  static void ParseAndPush(Source& source, std::vector<bool>& out) {
    bool item{};
    BoolAdapter::Parse(source, item);
    out.push_back(item);
  }

  template <typename Source, typename T>
  static void ParseAndPush(Source& source, std::vector<T>& out) {
    T& item = out.emplace_back();
    TypeAdapter<T>::Parse(source, item);
  }

  template <typename Source, typename T, typename GetKey>
  static void ParseAndPush(Source& source, soia::keyed_items<T, GetKey>& out) {
    T item{};
    TypeAdapter<T>::Parse(source, item);
    out.push_back(std::move(item));
  }
};

template <typename T>
inline ArrayAdapter GetAdapter(soia_type<std::vector<T>>);

template <typename T, typename GetKey>
inline ArrayAdapter GetAdapter(soia_type<soia::keyed_items<T, GetKey>>);

struct RecAdapter {
  template <typename T>
  static bool IsDefault(const soia::rec<T>& input) {
    return TypeAdapter<T>::IsDefault(*input);
  }

  template <typename T, typename Out>
  static void Append(const soia::rec<T>& input, Out& out) {
    TypeAdapter<T>::Append(*input, out);
  }

  template <typename T>
  static void Append(const soia::rec<T>& input, ByteSink& out) {
    return TypeAdapter<T>::Append(*input, out);
  }

  template <typename T>
  static void Parse(JsonTokenizer& tokenizer, soia::rec<T>& out) {
    TypeAdapter<T>::Parse(tokenizer, *out);
  }

  template <typename T>
  static void Parse(ByteSource& source, soia::rec<T>& out) {
    return TypeAdapter<T>::Parse(source, *out);
  }

  template <typename T>
  static soia::reflection::Type GetType(soia_type<soia::rec<T>>) {
    return soia_internal::GetType<T>();
  }

  template <typename T>
  static void RegisterRecords(soia_type<soia::rec<T>>,
                              soia::reflection::RecordRegistry& registry) {
    soia_internal::RegisterRecords<T>(registry);
  }

  static constexpr bool IsStruct() { return false; }
  static constexpr bool IsEnum() { return false; }
};

template <typename T>
inline RecAdapter GetAdapter(soia_type<soia::rec<T>>);

class JsonObjectWriter {
 public:
  JsonObjectWriter(ReadableJson* out) : out_(*out) {}

  ~JsonObjectWriter() {
    if (has_content_) {
      out_.out += out_.new_line.Dedent();
      out_.out += '}';
    } else {
      out_.out += {'{', '}'};
    }
  }

  template <typename T>
  JsonObjectWriter& Write(const char* field_name, const T& value) {
    if (!TypeAdapter<T>::IsDefault(value)) {
      if (has_content_) {
        out_.out += ',';
        out_.out += *out_.new_line;
      } else {
        out_.out += "{";
        out_.out += out_.new_line.Indent();
        has_content_ = true;
      }
      out_.out += '"';
      out_.out += field_name;
      out_.out += "\": ";
      TypeAdapter<T>::Append(value, out_);
    }
    return *this;
  }

 private:
  ReadableJson& out_;
  bool has_content_ = false;
};

class StructJsonObjectParserImpl {
 public:
  template <typename T, typename field_value>
  void AddField(const std::string& name, field_value T::* data_member) {
    fields_[name] = std::make_unique<FieldImpl<T, field_value>>(data_member);
  }

  void Parse(JsonTokenizer& tokenizer, void* out) const;

 private:
  struct Field {
    virtual ~Field() = default;
    virtual void Parse(JsonTokenizer& tokenizer, void* out) const = 0;
  };
  absl::flat_hash_map<std::string, std::unique_ptr<Field>> fields_;

  template <typename T, typename field_value>
  struct FieldImpl : public Field {
    FieldImpl(field_value T::* data_member) : data_member_(data_member) {}
    void Parse(JsonTokenizer& tokenizer, void* out) const override {
      ::soia_internal::Parse(tokenizer, static_cast<T*>(out)->*data_member_);
    }
    field_value T::* const data_member_;
  };
};

template <typename T>
class StructJsonObjectParser {
 public:
  template <typename field_value>
  StructJsonObjectParser* AddField(const std::string& name,
                                   field_value T::* data_member) {
    impl_.AddField<T>(name, data_member);
    return this;
  }

  void Parse(JsonTokenizer& tokenizer, T& out) const {
    impl_.Parse(tokenizer, &out);
  }

 private:
  StructJsonObjectParserImpl impl_;
};

class EnumJsonObjectParserImpl {
 public:
  template <typename T, typename WrapperType>
  void AddField(const std::string& name) {
    fields_[name] = std::make_unique<FieldImpl<T, WrapperType>>();
  }

  template <typename T, typename ValueType>
  void AddVariantField(const std::string& name) {
    fields_[name] = std::make_unique<VariantFieldImpl<T, ValueType>>();
  }

  void Parse(JsonTokenizer& tokenizer, void* out) const;

 private:
  struct Field {
    virtual ~Field() = default;
    virtual void Parse(JsonTokenizer& tokenizer, void* out) const = 0;
  };
  absl::flat_hash_map<std::string, std::unique_ptr<Field>> fields_;

  template <typename T, typename WrapperType>
  struct FieldImpl : public Field {
    void Parse(JsonTokenizer& tokenizer, void* out) const override {
      WrapperType wrapper;
      ::soia_internal::Parse(tokenizer, wrapper.value);
      *static_cast<T*>(out) = std::move(wrapper);
    }
  };

  template <typename T, typename ValueType>
  struct VariantFieldImpl : public Field {
    void Parse(JsonTokenizer& tokenizer, void* out) const override {
      ValueType value;
      ::soia_internal::Parse(tokenizer, value);
      *static_cast<T*>(out) = std::move(value);
    }
  };
};

template <typename T>
class EnumJsonObjectParser {
 public:
  template <typename WrapperType>
  EnumJsonObjectParser* AddField(const std::string& name) {
    impl_.AddField<T, WrapperType>(name);
    return this;
  }

  template <typename ValueType>
  EnumJsonObjectParser* AddVariantField(const std::string& name) {
    impl_.AddVariantField<T, ValueType>(name);
    return this;
  }

  void Parse(JsonTokenizer& tokenizer, T& out) const {
    impl_.Parse(tokenizer, &out);
  }

 private:
  EnumJsonObjectParserImpl impl_;
};

class EnumJsonArrayParser {
 public:
  EnumJsonArrayParser(JsonTokenizer* tokenizer);

  int ReadNumber();
  void Finish();

 private:
  JsonTokenizer& tokenizer_;
};

class DebugObjectWriter {
 public:
  DebugObjectWriter(DebugString* out) : out_(*out) {}

  ~DebugObjectWriter() {
    if (has_content_) {
      out_.out += out_.new_line.Dedent();
      out_.out += '}';
    } else {
      out_.out += {'{', '}'};
    }
  }

  template <typename T>
  DebugObjectWriter& Write(const char* field_name, const T& value) {
    if (!TypeAdapter<T>::IsDefault(value)) {
      if (has_content_) {
        out_.out += *out_.new_line;
      } else {
        out_.out += "{";
        out_.out += out_.new_line.Indent();
        has_content_ = true;
      }
      out_.out += '.';
      out_.out += field_name;
      out_.out += ": ";
      TypeAdapter<T>::Append(value, out_);
      out_.out += ',';
    }
    return *this;
  }

 private:
  DebugString& out_;
  bool has_content_ = false;
};

std::pair<bool, int32_t> ParseEnumPrefix(ByteSource& source);

class UnrecognizedValues {
 public:
  void ParseFrom(JsonTokenizer& tokenizer);
  void ParseFrom(ByteSource& source);
  void AppendTo(DenseJson& out) const;
  void AppendTo(ByteSink& out) const;

 private:
  ByteSink bytes_;
  std::vector<uint32_t> array_lengths_;
};

struct UnrecognizedFieldsData {
  uint32_t array_len = 0;
  UnrecognizedValues values;
};

template <typename Adapter>
struct UnrecognizedFields {
 private:
  std::shared_ptr<UnrecognizedFieldsData> data;
  friend Adapter;
};

struct UnrecognizedEnum {
  int32_t number = 0;
  std::shared_ptr<UnrecognizedValues> value;

  UnrecognizedValues& emplace_value() {
    return *(value = std::make_shared<UnrecognizedValues>());
  }
};

void AppendUnrecognizedEnum(const UnrecognizedEnum& input, DenseJson& out);
void AppendUnrecognizedEnum(const UnrecognizedEnum& input, ByteSink& out);

void ParseUnrecognizedFields(JsonArrayReader& array_reader, size_t num_slots,
                             size_t num_slots_incl_removed,
                             std::shared_ptr<UnrecognizedFieldsData>& out);

void ParseUnrecognizedFields(ByteSource& source, size_t array_len,
                             size_t num_slots, size_t num_slots_incl_removed,
                             std::shared_ptr<UnrecognizedFieldsData>& out);

template <typename HttplibHeaders>
void SoiaToHttplibHeaders(const soia::service::HttpHeaders& input,
                          HttplibHeaders& out) {
  for (const auto& [name, values] : input.map()) {
    for (const absl::string_view value : values) {
      out.insert({std::string(name), std::string(value)});
    }
  }
}

template <typename HttplibHeaders>
soia::service::HttpHeaders HttplibToSoiaHeaders(const HttplibHeaders& input) {
  soia::service::HttpHeaders result;
  for (const auto& [name, value] : input) {
    result.Insert(name, value);
  }
  return result;
}

template <typename MethodsTuple, std::size_t... Indices>
constexpr bool unique_method_numbers_impl(std::index_sequence<Indices...>) {
  constexpr std::array<int, sizeof...(Indices)> numbers = {
      std::get<Indices>(MethodsTuple()).kNumber...};
  for (std::size_t i = 0; i < sizeof...(Indices); ++i) {
    for (std::size_t j = i + 1; j < sizeof...(Indices); ++j) {
      if (numbers[i] == numbers[j]) {
        return false;
      }
    }
  }
  return true;
}

template <typename MethodsTuple>
constexpr bool unique_method_numbers() {
  return unique_method_numbers_impl<MethodsTuple>(
      std::make_index_sequence<std::tuple_size_v<MethodsTuple>>{});
}

template <typename MethodsTuple>
void assert_unique_method_numbers() {
  static_assert(unique_method_numbers<MethodsTuple>(),
                "Method numbers are not unique");
}

}  // namespace soia_internal

namespace soia {

// Deserializes a soia value.
// The input string can either be:
//   - the JSON returned by soia::ToDenseJson or soia::ToReadableJson
//   - the bytes returned by soia::ToBytes
template <typename T>
absl::StatusOr<T> Parse(absl::string_view bytes_or_json,
                        UnrecognizedFieldsPolicy unrecognized_fields =
                            UnrecognizedFieldsPolicy::kDrop) {
  T result{};
  if (bytes_or_json.length() >= 4 && bytes_or_json[0] == 's' &&
      bytes_or_json[1] == 'o' && bytes_or_json[2] == 'i' &&
      bytes_or_json[3] == 'a') {
    bytes_or_json = bytes_or_json.substr(4);
    soia_internal::ByteSource byte_source(bytes_or_json.data(),
                                          bytes_or_json.length());
    byte_source.keep_unrecognized_fields =
        unrecognized_fields == UnrecognizedFieldsPolicy::kKeep;
    soia_internal::Parse(byte_source, result);
    if (byte_source.error || byte_source.pos < byte_source.end) {
      return absl::UnknownError("error while decoding soia value from bytes");
    }
  } else {
    soia_internal::JsonTokenizer tokenizer(
        bytes_or_json.begin(), bytes_or_json.end(), unrecognized_fields);
    tokenizer.Next();
    soia_internal::Parse(tokenizer, result);
    if (tokenizer.state().token_type != soia_internal::JsonTokenType::kStrEnd) {
      tokenizer.mutable_state().PushUnexpectedTokenError("end");
    }
    const absl::Status status = tokenizer.state().status;
    if (!status.ok()) return status;
  }
  return result;
}

// Serializes the given value to dense JSON format.
template <typename T>
std::string ToDenseJson(const T& input) {
  static_assert(!std::is_pointer<T>::value,
                "Can't pass a pointer to ToDenseJson");
  soia_internal::DenseJson dense_json;
  Append(input, dense_json);
  return std::move(dense_json).out;
}

inline std::string ToDenseJson(const char* input) {
  return ToDenseJson(std::string(input));
}

// Serializes the given value to readable JSON format.
template <typename T>
std::string ToReadableJson(const T& input) {
  static_assert(!std::is_pointer<T>::value,
                "Can't pass a pointer to ToReadableJson");
  soia_internal::ReadableJson readable_json;
  Append(input, readable_json);
  return std::move(readable_json).out;
}

inline std::string ToReadableJson(const char* input) {
  return ToReadableJson(std::string(input));
}

// Serializes the given value to binary format.
template <typename T>
ByteString ToBytes(const T& input) {
  static_assert(!std::is_pointer<T>::value, "Can't pass a pointer to ToBytes");
  soia_internal::ByteSink byte_sink;
  byte_sink.Push('s', 'o', 'i', 'a');
  Append(input, byte_sink);
  return std::move(byte_sink).ToByteString();
}

inline ByteString ToBytes(const char* input) {
  return ToBytes(std::string(input));
}

// Minimum absl::Time encodable as a soia timestamp.
// Equal to 100M days before the Unix EPOCH.
constexpr absl::Time kMinEncodedTimestamp =
    absl::FromUnixMillis(-8640000000000000);
// Maximum absl::Time encodable as a soia timestamp.
// Equal to 100M days before the Unix EPOCH.
constexpr absl::Time kMaxEncodedTimestamp =
    absl::FromUnixMillis(8640000000000000);

namespace reflection {

template <typename T>
const TypeDescriptor& GetTypeDescriptor() {
  static const TypeDescriptor* result = []() -> const TypeDescriptor* {
    RecordRegistry registry;
    soia_internal::RegisterRecords<T>(registry);
    return new TypeDescriptor{soia_internal::GetType<T>(), std::move(registry)};
  }();
  return *result;
}

template <typename T>
constexpr bool IsStruct() {
  return soia_internal::TypeAdapter<T>::IsStruct();
}

template <typename T>
constexpr bool IsEnum() {
  return soia_internal::TypeAdapter<T>::IsEnum();
}

template <typename T>
constexpr bool IsRecord() {
  return IsStruct<T>() || IsEnum<T>();
}

// Calls f(field) for each field in the soia-generated datatype.
// The type of the single argument passed to f is one of: struct_field,
// enum_const_field, enum_value_field.
template <typename Record, typename F>
void ForEachField(F&& f) {
  static_assert(IsStruct<Record>() || IsEnum<Record>());
  std::apply(
      [&f](auto&&... x) {
        (static_cast<void>(f(std::forward<decltype(x)>(x))), ...);
      },
      typename soia_internal::TypeAdapter<Record>::fields_tuple());
}

}  // namespace reflection
}  // namespace soia

namespace soia_internal {

absl::StatusOr<std::tuple<absl::string_view, absl::string_view,
                          absl::string_view, absl::string_view>>
SplitRequestBody(absl::string_view request_body);

absl::StatusOr<int> ParseMethodNumber(absl::string_view method_number_str);

struct MethodDescriptor {
  absl::string_view name;
  int number = 0;
  std::string request_descriptor_json;
  std::string response_descriptor_json;
};

template <typename Method>
MethodDescriptor MakeMethodDescriptor(Method method) {
  MethodDescriptor result;
  result.name = Method::kMethodName;
  result.number = Method::kNumber;
  result.request_descriptor_json =
      soia::reflection::GetTypeDescriptor<typename Method::request_type>()
          .AsJson();
  result.response_descriptor_json =
      soia::reflection::GetTypeDescriptor<typename Method::response_type>()
          .AsJson();
  return result;
}

std::string MethodListToJson(const std::vector<MethodDescriptor>&);

template <typename ServiceImpl, typename RequestMeta, typename ResponseMeta>
class HandleRequestOp {
 public:
  HandleRequestOp(ServiceImpl* service_impl, absl::string_view request_body,
                  std::function<absl::optional<std::string>(absl::string_view)>
                      get_query_param_fn,
                  soia::UnrecognizedFieldsPolicy unrecognized_fields,
                  const RequestMeta* request_meta, ResponseMeta* response_meta)
      : service_impl_(*service_impl),
        request_body_(request_body),
        get_query_param_fn_(get_query_param_fn),
        unrecognized_fields_(unrecognized_fields),
        request_meta_(*request_meta),
        response_meta_(*response_meta) {}

  soia::service::RawResponse Run() {
    if (get_query_param_fn_("list").has_value() || request_body_ == "list") {
      std::vector<MethodDescriptor> method_descriptors;
      std::apply(
          [&](auto... method) {
            (method_descriptors.push_back(MakeMethodDescriptor(method)), ...);
          },
          typename ServiceImpl::methods());
      return {
          MethodListToJson(method_descriptors),
          soia::service::ResponseType::kOkJson,
      };
    }

    std::string method_name;
    std::string method_number_str;
    if (const auto m = get_query_param_fn_("m"); m.has_value()) {
      method_name = get_query_param_fn_("method").value_or("");
      method_number_str = *m;
      format_ = get_query_param_fn_("f").value_or("");
      request_data_buffer_ = get_query_param_fn_("req").value_or("");
      request_data_ = request_data_buffer_;
    } else {
      const auto parts = SplitRequestBody(request_body_);
      if (!parts.ok()) {
        return {
            absl::StrCat("bad request: ", parts.status().message()),
            soia::service::ResponseType::kBadRequest,
        };
      }
      method_name = std::get<0>(*parts);
      method_number_str = std::get<1>(*parts);
      format_ = std::get<2>(*parts);
      request_data_ = std::get<3>(*parts);
    }

    const absl::StatusOr<int> method_number =
        ParseMethodNumber(method_number_str);
    if (!method_number.ok()) {
      return {
          absl::StrCat("bad request: ", method_number.status().message()),
          soia::service::ResponseType::kBadRequest,
      };
    }
    method_number_ = *method_number;

    std::apply(
        [&](auto... method) {
          (static_cast<void>(MaybeInvokeMethod(method)), ...);
        },
        typename ServiceImpl::methods());
    if (raw_response_.has_value()) {
      return std::move(*raw_response_);
    }
    return {absl::StrCat("bad request: method not found: ", method_name,
                         "; number: ", method_number_),
            soia::service::ResponseType::kBadRequest};
  }

 private:
  ServiceImpl& service_impl_;
  const absl::string_view request_body_;
  const std::function<absl::optional<std::string>(absl::string_view)>
      get_query_param_fn_;
  const soia::UnrecognizedFieldsPolicy unrecognized_fields_;
  const RequestMeta& request_meta_;
  ResponseMeta& response_meta_;

  int method_number_ = 0;
  std::string format_;
  std::string request_data_buffer_;
  absl::string_view request_data_;

  absl::optional<soia::service::RawResponse> raw_response_;

  template <typename Method>
  void MaybeInvokeMethod(Method method) {
    using MethodType = decltype(method);
    using RequestType = typename MethodType::request_type;
    using ResponseType = typename MethodType::response_type;
    if (MethodType::kNumber != method_number_) return;
    raw_response_.emplace();
    absl::StatusOr<RequestType> request =
        soia::Parse<RequestType>(request_data_, unrecognized_fields_);
    if (!request.ok()) {
      raw_response_->data =
          absl::StrCat("bad request: ", request.status().message());
      raw_response_->type = soia::service::ResponseType::kBadRequest;
      return;
    }
    absl::StatusOr<ResponseType> output = service_impl_(
        method, std::move(*request), request_meta_, response_meta_);
    if (!output.ok()) {
      raw_response_->data =
          absl::StrCat("server error: ", output.status().message());
      raw_response_->type = soia::service::ResponseType::kServerError;
      return;
    }
    if (format_ == "readable") {
      raw_response_->data = soia::ToReadableJson(*output);
      raw_response_->type = soia::service::ResponseType::kOkJson;
    } else {
      raw_response_->data = soia::ToDenseJson(*output);
      raw_response_->type = soia::service::ResponseType::kOkJson;
    }
  }
};

template <typename HttplibClientPtr>
class HttplibClient : public soia::service::Client {
 public:
  HttplibClient(HttplibClientPtr client, std::string query_path)
      : client_(std::move(ABSL_DIE_IF_NULL(client))), query_path_(query_path) {}

  absl::StatusOr<std::string> operator()(
      absl::string_view request_data,
      const soia::service::HttpHeaders& request_headers,
      soia::service::HttpHeaders& response_headers) const {
    auto headers =
        decltype(std::declval<HttplibClientPtr>()->Get("")->headers)();
    SoiaToHttplibHeaders(request_headers, headers);
    auto result = client_->Post(query_path_, headers, request_data.data(),
                                request_data.length(), "text/plain");
    if (result) {
      response_headers = HttplibToSoiaHeaders(result->headers);
      const int status_code = result->status;
      if (200 <= status_code && status_code <= 299) {
        // OK status.
        return std::move(result->body);
      } else {
        const bool body_is_text =
            result->get_header_value("Content-Type") == "text/plain";
        return absl::UnknownError(absl::StrCat(
            "HTTP response status ", status_code, body_is_text ? ": " : "",
            body_is_text ? result->body : ""));
      }
    } else {
      // HTTP error.
      response_headers = {};
      std::stringstream ss;
      ss << "HTTP error: " << result.error();
      return {absl::UnknownError(ss.str())};
    }
  }

 private:
  const HttplibClientPtr client_;
  const std::string query_path_;
};

}  // namespace soia_internal

namespace soia {
namespace service {

// On the server side, parses the content of a user request and invokes the
// appropriate method on the given service implementation.
//
// ServiceImpl must satisfy the following requirements:
//   1. It must have a public `methods` type alias which resolves to a tuple of
// //     method types.
//   2. For each method, it must have a member function with this signature:
//        absl::StatusOr<typename Method::response_type> operator()(
//            Method method,
//            typename Method::request_type request,
//            const HttpHeaders& request_headers,
//            HttpHeaders& response_headers);
//
// For example:
//
//   class MyServiceImpl {
//    public:
//     using methods = std::tuple<
//          soiagen_methods::ListUsers,
//          soiagen_methods::GetUser>;
//
//     absl::StatusOr<soiagen_methods::ListUsersResponse> operator()(
//         soiagen_methods::ListUsers,
//         soiagen_methods::ListUsersRequest request,
//         const HttpHeaders& request_headers,
//         HttpHeaders& response_headers) const {
//       ...
//     }
//
//     absl::StatusOr<soiagen_methods::GetUserResponse> operator()(
//         soiagen_methods::GetUser,
//         soiagen_methods::GetUserRequest request,
//         const HttpHeaders& request_headers,
//         HttpHeaders& response_headers) const {
//       ...
//     }
//   };
//
// If you are using cpp-httplib (https://github.com/yhirose/cpp-httplib) as
// your server library, you don't need to call HandleRequest, you can simply
// call InstallServiceOnHttplibServer. If you are using another server library,
// call HandleRequest in the logic for installing a soia service on your
// server.
//
// Pass in UnrecognizedFieldsPolicy::kKeep if the request cannot come from a
// malicious user.
template <typename ServiceImpl>
RawResponse HandleRequest(
    ServiceImpl& service_impl, absl::string_view request_body,
    std::function<absl::optional<std::string>(absl::string_view)>
        get_query_param_fn,
    const HttpHeaders& request_headers, HttpHeaders& response_headers,
    UnrecognizedFieldsPolicy unrecognized_fields =
        UnrecognizedFieldsPolicy::kDrop) {
  soia_internal::assert_unique_method_numbers<typename ServiceImpl::methods>();
  return soia_internal::HandleRequestOp(&service_impl, request_body,
                                        get_query_param_fn, unrecognized_fields,
                                        &request_headers, &response_headers)
      .Run();
}

// Installs a soia service on the given httplib::Server as the given query
// path. The httplib::Server type is referred to as a template parameter so as
// not to make cpp-httplib a dependency of soia.
//
// ServiceImpl must satisfy the requirements outlined in the documentation for
// HandleRequest.
//
// Pass in UnrecognizedFieldsPolicy::kKeep if the requests cannot come from a
// malicious user.
template <typename HttplibServer, typename ServiceImpl>
void InstallServiceOnHttplibServer(
    HttplibServer& server, absl::string_view query_path,
    std::shared_ptr<ServiceImpl> service_impl,
    UnrecognizedFieldsPolicy unrecognized_fields =
        UnrecognizedFieldsPolicy::kDrop) {
  ABSL_CHECK_NE(service_impl, nullptr);
  const typename HttplibServer::Handler handler =  //
      [service_impl, unrecognized_fields](const auto& req, auto& resp) {
        const HttpHeaders request_headers =
            soia_internal::HttplibToSoiaHeaders(req.headers);
        HttpHeaders response_headers;
        RawResponse raw_response = HandleRequest(
            *service_impl, req.body,
            [&req](absl::string_view name) {
              const std::string name_str(name);
              return req.has_param(name_str)
                         ? absl::make_optional(req.get_param_value(name_str))
                         : absl::nullopt;
            },
            request_headers, response_headers, unrecognized_fields);
        soia_internal::SoiaToHttplibHeaders(response_headers, resp.headers);

        resp.set_content(std::move(raw_response.data),
                         std::string(raw_response.content_type()));
        resp.status = raw_response.status_code();
      };
  server.Get(std::string(query_path), handler);
  server.Post(std::string(query_path), handler);
}

// Invokes the given method on a remote server by making an RPC.
// Returns an error status if there was a network error or if the server
// returned an error.
template <typename Method>
absl::StatusOr<typename Method::response_type> InvokeRemote(
    const Client& client, Method method,
    const typename Method::request_type& request,
    const HttpHeaders& request_headers = {},
    HttpHeaders* response_headers = nullptr) {
  const std::string request_data = absl::StrCat(
      Method::kMethodName, ":", Method::kNumber, "::", ToDenseJson(request));
  HttpHeaders response_headers_tmp;
  absl::StatusOr<std::string> response_data =
      client(request_data, request_headers, response_headers_tmp);
  if (response_headers != nullptr) {
    *response_headers = std::move(response_headers_tmp);
  }
  if (!response_data.ok()) {
    return std::move(response_data).status();
  }
  return Parse<typename Method::response_type>(*response_data,
                                               UnrecognizedFieldsPolicy::kKeep);
}

// Returns a client for sending RPCs to a soia service via the given
// httplib::Client.
// The httplib::Client type is referred to as a template parameter so as not to
// make cpp-httplib a dependency of soia.
//
// If you are not using cpp-httplib, you can write your own Client
// implementation.
template <typename HttplibClient>
std::unique_ptr<Client> MakeHttplibClient(absl::Nonnull<HttplibClient*> client,
                                          absl::string_view query_path) {
  return std::make_unique<soia_internal::HttplibClient<HttplibClient*>>(
      client, std::string(query_path));
};

// Returns a client for sending RPCs to a soia service via the given
// httplib::Client.
// The httplib::Client type is referred to as a template parameter so as not to
// make cpp-httplib a dependency of soia.
//
// If you are not using cpp-httplib, you can write your own Client
// implementation.
template <typename HttplibClient>
std::unique_ptr<Client> MakeHttplibClient(std::unique_ptr<HttplibClient> client,
                                          absl::string_view query_path) {
  return std::make_unique<
      soia_internal::HttplibClient<std::unique_ptr<HttplibClient>>>(
      std::move(client), std::string(query_path));
};

}  // namespace service
}  // namespace soia

#endif
