// Copyright 2015 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef SYZYGY_REFINERY_TYPES_TYPE_H_
#define SYZYGY_REFINERY_TYPES_TYPE_H_

#include <stdint.h>
#include <functional>
#include <vector>

#include "base/logging.h"
#include "base/macros.h"
#include "base/memory/ref_counted.h"
#include "base/strings/string16.h"

namespace refinery {

// fwd.
class TypeRepository;
typedef size_t TypeId;

// A sentinel value for uninitialized types.
const TypeId kNoTypeId = static_cast<TypeId>(-1);

// A base class for all Type subclasses. Types are owned by a type repository,
// which can vend out type instances by ID on demand.
class Type : public base::RefCounted<Type> {
 public:
  typedef uint8_t Flags;

  // The set of type classes is closed, each type is enumerated here.
  enum TypeKind {
    BASIC_TYPE_KIND,
    USER_DEFINED_TYPE_KIND,
    POINTER_TYPE_KIND,
    ARRAY_TYPE_KIND,
    WILDCARD_TYPE_KIND,
  };

  enum CV_FLAGS {
    FLAG_CONST        = 0x0001,
    FLAG_VOLATILE     = 0x0002,
  };

  // @name Accessors
  // @{
  TypeRepository* repository() const { return repository_; }
  TypeId type_id() const { return type_id_; }
  const base::string16& name() const { return name_; }
  const base::string16& decorated_name() const { return decorated_name_; }
  size_t size() const { return size_; }
  TypeKind kind() const { return kind_; }
  // @}

  // Safely down-cast this to @p SubType.
  // @param out the subtype to cast this to.
  // @returns true on success, false on failure.
  template <class SubType>
  bool CastTo(scoped_refptr<SubType>* out);

 protected:
  friend class base::RefCounted<Type>;

  // This constructor will eventually be removed.
  Type(TypeKind kind, const base::string16& name, size_t size);
  // This is the way to go.
  Type(TypeKind kind,
       const base::string16& name,
       const base::string16& decorated_name,
       size_t size);
  virtual ~Type() = 0;

  // Name of type.
  // This is protected as opposed to private to allow setting it
  // post-construction.
  base::string16 name_;

  // Decorated name of type.
  base::string16 decorated_name_;

 private:
  friend class TypeRepository;
  void SetRepository(TypeRepository* repository, TypeId type_id);

  // The type repository this type belongs to and its ID in the repository.
  TypeRepository* repository_;
  TypeId type_id_;

  // The kind of this type is, synonymous with its class.
  const TypeKind kind_;
  // Size of type.
  const size_t size_;

  DISALLOW_COPY_AND_ASSIGN(Type);
};

using TypePtr = scoped_refptr<Type>;

// Constant for no type flags.
const Type::Flags kNoTypeFlags = 0x0000;

// Represents a basic type, such as e.g. an int, char, void, etc.
class BasicType : public Type {
 public:
  static const TypeKind ID = BASIC_TYPE_KIND;

  // Creates a new basictype with name @p name and size @p size.
  // Sets decorated_name equal to name as basic types have no decorated names.
  BasicType(const base::string16& name, size_t size);

 private:
  DISALLOW_COPY_AND_ASSIGN(BasicType);
};

using BasicTypePtr = scoped_refptr<BasicType>;

// Represents a user defined type such as a struct, union or a class.
class UserDefinedType : public Type {
 public:
  class Field;
  typedef std::vector<Field> Fields;

  static const TypeKind ID = USER_DEFINED_TYPE_KIND;

  // Creates a new user defined type with name @p name, size @p size.
  // This creates an un-finalized UDT with no fields.
  // This will eventually be deleted.
  UserDefinedType(const base::string16& name, size_t size);

  // Creates a new user defined type with name @p name, decorated name @p
  // decorated_name and size @p size.
  // This creates an un-finalized UDT with no fields.
  UserDefinedType(const base::string16& name,
                  const base::string16& decorated_name,
                  size_t size);

  // Retrieves the type associated with field @p field_no.
  // @pre field_no < fields().size().
  // @pre SetRepository has been called.
  TypePtr GetFieldType(size_t field_no) const;

  // Accessor.
  const Fields& fields() const { return fields_; }

  // Finalize the type by providing it with a field list.
  // @param fields the fields for the type.
  // @note this can only be called once per type instance.
  void Finalize(const Fields& fields);

 private:
  Fields fields_;

  DISALLOW_COPY_AND_ASSIGN(UserDefinedType);
};

using UserDefinedTypePtr = scoped_refptr<UserDefinedType>;

// Represents a field in a user defined type.
class UserDefinedType::Field {
 public:
  // TODO(siggi): How to represent VTables/Interfaces?

  // Creates a new field.
  // @param name the name of the field.
  // @param offset the byte offset of the field within the UDT.
  //    Note that many bitfield fields can share the same offset within a UDT,
  //    as can fields in a union.
  // @param flags any combination of Flags, denoting properties of the field.
  // @param bit_pos if this field is a bitfield, this is the bit position.
  // @param bit_len if this field is a bitfield, this is the bit length.
  // @param type_id the type ID of the field.
  // @note bit_pos and bit_len must be in the range 0..63.
  // @note When bit_len is zero it signifies that the field is not a bitfield.
  Field(const base::string16& name,
        ptrdiff_t offset,
        Flags flags,
        size_t bit_pos,
        size_t bit_len,
        TypeId type_id);

  // @name Accessors.
  // @{
  const base::string16& name() const { return name_; }
  ptrdiff_t offset() const { return offset_; }
  TypeId type_id() const { return type_id_; }
  size_t bit_pos() const { return bit_pos_; }
  size_t bit_len() const { return bit_len_; }
  bool is_const() const { return (flags_ & FLAG_CONST) != 0; }
  bool is_volatile() const { return (flags_ & FLAG_VOLATILE) != 0; }
  // @}

  bool operator==(const Field& o) const;

 private:
  const base::string16 name_;
  const ptrdiff_t offset_;
  const Flags flags_;
  const size_t bit_pos_ : 6;
  const size_t bit_len_ : 6;
  const TypeId type_id_;
};

// Represents a pointer to some other type.
class PointerType : public Type {
 public:
  static const TypeKind ID = POINTER_TYPE_KIND;

  // Creates a new (non-finalized) pointer type with size @p size.
  explicit PointerType(size_t size);

  // Creates a new (non-finalized) pointer type with name @p name, decorated
  // name @p decorated_name and size @p size.
  PointerType(const base::string16& name,
              const base::string16& decorated_name,
              size_t size);

  // Accessors.
  // @{
  TypeId content_type_id() const { return content_type_id_; }
  bool is_const() const { return (flags_ & FLAG_CONST) != 0; }
  bool is_volatile() const { return (flags_ & FLAG_VOLATILE) != 0; }
  // @}

  // Retrieves the type this pointer refers to.
  // @pre SetRepository has been called.
  TypePtr GetContentType() const;

  // Finalize the pointer type with @p flags and @p content_type_id.
  void Finalize(Flags flags, TypeId content_type_id);
  // Set the name of the pointer type.
  void SetName(const base::string16& name);

  // Set the name of the pointer type.
  void SetDecoratedName(const base::string16& decorated_name);

 private:
  // Stores the CV qualifiers of the pointee.
  Flags flags_;
  // Stores the type this pointer points to.
  TypeId content_type_id_;
};

using PointerTypePtr = scoped_refptr<PointerType>;

// Represents an array of some other type.
class ArrayType : public Type {
 public:
  static const TypeKind ID = ARRAY_TYPE_KIND;

  explicit ArrayType(size_t size);

  // Accessors.
  // @{
  TypeId index_type_id() const { return index_type_id_; }
  size_t num_elements() const { return num_elements_; }
  TypeId element_type_id() const { return element_type_id_; }

  bool is_const() const { return (flags_ & FLAG_CONST) != 0; }
  bool is_volatile() const { return (flags_ & FLAG_VOLATILE) != 0; }
  // @}

  // @name Retrieve the index/element types.
  // @pre SetRepository has been called.
  // @{
  TypePtr GetIndexType() const;
  TypePtr GetElementType() const;
  // @}

  // Finalize the array type.
  void Finalize(Flags flags,
                TypeId index_type_id,
                size_t num_elements,
                TypeId element_type_id);
  // Set the name of the array type.
  void SetName(const base::string16& name);

 private:
  // The CV qualifiers for the elements.
  Flags flags_;

  // The type ID for the the index type.
  TypeId index_type_id_;

  // The number of elements in this array.
  size_t num_elements_;

  // The type ID for the element type.
  TypeId element_type_id_;
};

using ArrayTypePtr = scoped_refptr<ArrayType>;

// Represents an otherwise unsupported type.
// TODO(siggi): This is a stub, which needs to go away ASAP.
class WildcardType : public Type {
 public:
  static const TypeKind ID = WILDCARD_TYPE_KIND;

  // Creates a new wildcard type with name @p name, size @p size.
  WildcardType(const base::string16& name, size_t size);
  // Creates a new wildcard type with name @p name, @p decorated_name and
  // size @p size.
  WildcardType(const base::string16& name,
               const base::string16& decorated_name,
               size_t size);
};

using WildcardTypePtr = scoped_refptr<WildcardType>;

template <class SubType>
bool Type::CastTo(scoped_refptr<SubType>* out) {
  DCHECK(out);
  if (SubType::ID != kind()) {
    *out = nullptr;
    return false;
  }

  *out = static_cast<SubType*>(this);
  return true;
}

}  // namespace refinery

#endif  // SYZYGY_REFINERY_TYPES_TYPE_H_
