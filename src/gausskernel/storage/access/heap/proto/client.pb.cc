// Generated by the protocol buffer compiler.  DO NOT EDIT!
// source: client.proto

#include "client.pb.h"

#include <algorithm>

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/extension_set.h>
#include <google/protobuf/wire_format_lite.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/generated_message_reflection.h>
#include <google/protobuf/reflection_ops.h>
#include <google/protobuf/wire_format.h>
// @@protoc_insertion_point(includes)
#include <google/protobuf/port_def.inc>
extern PROTOBUF_INTERNAL_EXPORT_transaction_2eproto ::PROTOBUF_NAMESPACE_ID::internal::SCCInfo<1> scc_info_Row_transaction_2eproto;
namespace proto {
class ClientReadRequestDefaultTypeInternal {
 public:
  ::PROTOBUF_NAMESPACE_ID::internal::ExplicitlyConstructed<ClientReadRequest> _instance;
} _ClientReadRequest_default_instance_;
class ClientReadResponseDefaultTypeInternal {
 public:
  ::PROTOBUF_NAMESPACE_ID::internal::ExplicitlyConstructed<ClientReadResponse> _instance;
} _ClientReadResponse_default_instance_;
}  // namespace proto
static void InitDefaultsscc_info_ClientReadRequest_client_2eproto() {
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  {
    void* ptr = &::proto::_ClientReadRequest_default_instance_;
    new (ptr) ::proto::ClientReadRequest();
    ::PROTOBUF_NAMESPACE_ID::internal::OnShutdownDestroyMessage(ptr);
  }
  ::proto::ClientReadRequest::InitAsDefaultInstance();
}

::PROTOBUF_NAMESPACE_ID::internal::SCCInfo<1> scc_info_ClientReadRequest_client_2eproto =
    {{ATOMIC_VAR_INIT(::PROTOBUF_NAMESPACE_ID::internal::SCCInfoBase::kUninitialized), 1, 0, InitDefaultsscc_info_ClientReadRequest_client_2eproto}, {
      &scc_info_Row_transaction_2eproto.base,}};

static void InitDefaultsscc_info_ClientReadResponse_client_2eproto() {
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  {
    void* ptr = &::proto::_ClientReadResponse_default_instance_;
    new (ptr) ::proto::ClientReadResponse();
    ::PROTOBUF_NAMESPACE_ID::internal::OnShutdownDestroyMessage(ptr);
  }
  ::proto::ClientReadResponse::InitAsDefaultInstance();
}

::PROTOBUF_NAMESPACE_ID::internal::SCCInfo<1> scc_info_ClientReadResponse_client_2eproto =
    {{ATOMIC_VAR_INIT(::PROTOBUF_NAMESPACE_ID::internal::SCCInfoBase::kUninitialized), 1, 0, InitDefaultsscc_info_ClientReadResponse_client_2eproto}, {
      &scc_info_Row_transaction_2eproto.base,}};

static ::PROTOBUF_NAMESPACE_ID::Metadata file_level_metadata_client_2eproto[2];
static constexpr ::PROTOBUF_NAMESPACE_ID::EnumDescriptor const** file_level_enum_descriptors_client_2eproto = nullptr;
static constexpr ::PROTOBUF_NAMESPACE_ID::ServiceDescriptor const** file_level_service_descriptors_client_2eproto = nullptr;

const ::PROTOBUF_NAMESPACE_ID::uint32 TableStruct_client_2eproto::offsets[] PROTOBUF_SECTION_VARIABLE(protodesc_cold) = {
  ~0u,  // no _has_bits_
  PROTOBUF_FIELD_OFFSET(::proto::ClientReadRequest, _internal_metadata_),
  ~0u,  // no _extensions_
  ~0u,  // no _oneof_case_
  ~0u,  // no _weak_field_map_
  PROTOBUF_FIELD_OFFSET(::proto::ClientReadRequest, client_ip_),
  PROTOBUF_FIELD_OFFSET(::proto::ClientReadRequest, txn_id_),
  PROTOBUF_FIELD_OFFSET(::proto::ClientReadRequest, rows_),
  ~0u,  // no _has_bits_
  PROTOBUF_FIELD_OFFSET(::proto::ClientReadResponse, _internal_metadata_),
  ~0u,  // no _extensions_
  ~0u,  // no _oneof_case_
  ~0u,  // no _weak_field_map_
  PROTOBUF_FIELD_OFFSET(::proto::ClientReadResponse, result_),
  PROTOBUF_FIELD_OFFSET(::proto::ClientReadResponse, txn_id_),
  PROTOBUF_FIELD_OFFSET(::proto::ClientReadResponse, rows_),
};
static const ::PROTOBUF_NAMESPACE_ID::internal::MigrationSchema schemas[] PROTOBUF_SECTION_VARIABLE(protodesc_cold) = {
  { 0, -1, sizeof(::proto::ClientReadRequest)},
  { 8, -1, sizeof(::proto::ClientReadResponse)},
};

static ::PROTOBUF_NAMESPACE_ID::Message const * const file_default_instances[] = {
  reinterpret_cast<const ::PROTOBUF_NAMESPACE_ID::Message*>(&::proto::_ClientReadRequest_default_instance_),
  reinterpret_cast<const ::PROTOBUF_NAMESPACE_ID::Message*>(&::proto::_ClientReadResponse_default_instance_),
};

const char descriptor_table_protodef_client_2eproto[] PROTOBUF_SECTION_VARIABLE(protodesc_cold) =
  "\n\014client.proto\022\005proto\032\021transaction.proto"
  "\032\nnode.proto\"P\n\021ClientReadRequest\022\021\n\tcli"
  "ent_ip\030\001 \001(\t\022\016\n\006txn_id\030\002 \001(\004\022\030\n\004rows\030\003 \003"
  "(\0132\n.proto.Row\"]\n\022ClientReadResponse\022\035\n\006"
  "result\030\001 \001(\0162\r.proto.Result\022\016\n\006txn_id\030\002 "
  "\001(\004\022\030\n\004rows\030\003 \003(\0132\n.proto.RowB\016Z\014./taas_"
  "protob\006proto3"
  ;
static const ::PROTOBUF_NAMESPACE_ID::internal::DescriptorTable*const descriptor_table_client_2eproto_deps[2] = {
  &::descriptor_table_node_2eproto,
  &::descriptor_table_transaction_2eproto,
};
static ::PROTOBUF_NAMESPACE_ID::internal::SCCInfoBase*const descriptor_table_client_2eproto_sccs[2] = {
  &scc_info_ClientReadRequest_client_2eproto.base,
  &scc_info_ClientReadResponse_client_2eproto.base,
};
static ::PROTOBUF_NAMESPACE_ID::internal::once_flag descriptor_table_client_2eproto_once;
static bool descriptor_table_client_2eproto_initialized = false;
const ::PROTOBUF_NAMESPACE_ID::internal::DescriptorTable descriptor_table_client_2eproto = {
  &descriptor_table_client_2eproto_initialized, descriptor_table_protodef_client_2eproto, "client.proto", 253,
  &descriptor_table_client_2eproto_once, descriptor_table_client_2eproto_sccs, descriptor_table_client_2eproto_deps, 2, 2,
  schemas, file_default_instances, TableStruct_client_2eproto::offsets,
  file_level_metadata_client_2eproto, 2, file_level_enum_descriptors_client_2eproto, file_level_service_descriptors_client_2eproto,
};

// Force running AddDescriptors() at dynamic initialization time.
static bool dynamic_init_dummy_client_2eproto = (  ::PROTOBUF_NAMESPACE_ID::internal::AddDescriptors(&descriptor_table_client_2eproto), true);
namespace proto {

// ===================================================================

void ClientReadRequest::InitAsDefaultInstance() {
}
class ClientReadRequest::_Internal {
 public:
};

void ClientReadRequest::clear_rows() {
  rows_.Clear();
}
ClientReadRequest::ClientReadRequest()
  : ::PROTOBUF_NAMESPACE_ID::Message(), _internal_metadata_(nullptr) {
  SharedCtor();
  // @@protoc_insertion_point(constructor:proto.ClientReadRequest)
}
ClientReadRequest::ClientReadRequest(const ClientReadRequest& from)
  : ::PROTOBUF_NAMESPACE_ID::Message(),
      _internal_metadata_(nullptr),
      rows_(from.rows_) {
  _internal_metadata_.MergeFrom(from._internal_metadata_);
  client_ip_.UnsafeSetDefault(&::PROTOBUF_NAMESPACE_ID::internal::GetEmptyStringAlreadyInited());
  if (!from._internal_client_ip().empty()) {
    client_ip_.AssignWithDefault(&::PROTOBUF_NAMESPACE_ID::internal::GetEmptyStringAlreadyInited(), from.client_ip_);
  }
  txn_id_ = from.txn_id_;
  // @@protoc_insertion_point(copy_constructor:proto.ClientReadRequest)
}

void ClientReadRequest::SharedCtor() {
  ::PROTOBUF_NAMESPACE_ID::internal::InitSCC(&scc_info_ClientReadRequest_client_2eproto.base);
  client_ip_.UnsafeSetDefault(&::PROTOBUF_NAMESPACE_ID::internal::GetEmptyStringAlreadyInited());
  txn_id_ = PROTOBUF_ULONGLONG(0);
}

ClientReadRequest::~ClientReadRequest() {
  // @@protoc_insertion_point(destructor:proto.ClientReadRequest)
  SharedDtor();
}

void ClientReadRequest::SharedDtor() {
  client_ip_.DestroyNoArena(&::PROTOBUF_NAMESPACE_ID::internal::GetEmptyStringAlreadyInited());
}

void ClientReadRequest::SetCachedSize(int size) const {
  _cached_size_.Set(size);
}
const ClientReadRequest& ClientReadRequest::default_instance() {
  ::PROTOBUF_NAMESPACE_ID::internal::InitSCC(&::scc_info_ClientReadRequest_client_2eproto.base);
  return *internal_default_instance();
}


void ClientReadRequest::Clear() {
// @@protoc_insertion_point(message_clear_start:proto.ClientReadRequest)
  ::PROTOBUF_NAMESPACE_ID::uint32 cached_has_bits = 0;
  // Prevent compiler warnings about cached_has_bits being unused
  (void) cached_has_bits;

  rows_.Clear();
  client_ip_.ClearToEmptyNoArena(&::PROTOBUF_NAMESPACE_ID::internal::GetEmptyStringAlreadyInited());
  txn_id_ = PROTOBUF_ULONGLONG(0);
  _internal_metadata_.Clear();
}

const char* ClientReadRequest::_InternalParse(const char* ptr, ::PROTOBUF_NAMESPACE_ID::internal::ParseContext* ctx) {
#define CHK_(x) if (PROTOBUF_PREDICT_FALSE(!(x))) goto failure
  while (!ctx->Done(&ptr)) {
    ::PROTOBUF_NAMESPACE_ID::uint32 tag;
    ptr = ::PROTOBUF_NAMESPACE_ID::internal::ReadTag(ptr, &tag);
    CHK_(ptr);
    switch (tag >> 3) {
      // string client_ip = 1;
      case 1:
        if (PROTOBUF_PREDICT_TRUE(static_cast<::PROTOBUF_NAMESPACE_ID::uint8>(tag) == 10)) {
          auto str = _internal_mutable_client_ip();
          ptr = ::PROTOBUF_NAMESPACE_ID::internal::InlineGreedyStringParser(str, ptr, ctx);
          CHK_(::PROTOBUF_NAMESPACE_ID::internal::VerifyUTF8(str, "proto.ClientReadRequest.client_ip"));
          CHK_(ptr);
        } else goto handle_unusual;
        continue;
      // uint64 txn_id = 2;
      case 2:
        if (PROTOBUF_PREDICT_TRUE(static_cast<::PROTOBUF_NAMESPACE_ID::uint8>(tag) == 16)) {
          txn_id_ = ::PROTOBUF_NAMESPACE_ID::internal::ReadVarint(&ptr);
          CHK_(ptr);
        } else goto handle_unusual;
        continue;
      // repeated .proto.Row rows = 3;
      case 3:
        if (PROTOBUF_PREDICT_TRUE(static_cast<::PROTOBUF_NAMESPACE_ID::uint8>(tag) == 26)) {
          ptr -= 1;
          do {
            ptr += 1;
            ptr = ctx->ParseMessage(_internal_add_rows(), ptr);
            CHK_(ptr);
            if (!ctx->DataAvailable(ptr)) break;
          } while (::PROTOBUF_NAMESPACE_ID::internal::ExpectTag<26>(ptr));
        } else goto handle_unusual;
        continue;
      default: {
      handle_unusual:
        if ((tag & 7) == 4 || tag == 0) {
          ctx->SetLastTag(tag);
          goto success;
        }
        ptr = UnknownFieldParse(tag, &_internal_metadata_, ptr, ctx);
        CHK_(ptr != nullptr);
        continue;
      }
    }  // switch
  }  // while
success:
  return ptr;
failure:
  ptr = nullptr;
  goto success;
#undef CHK_
}

::PROTOBUF_NAMESPACE_ID::uint8* ClientReadRequest::_InternalSerialize(
    ::PROTOBUF_NAMESPACE_ID::uint8* target, ::PROTOBUF_NAMESPACE_ID::io::EpsCopyOutputStream* stream) const {
  // @@protoc_insertion_point(serialize_to_array_start:proto.ClientReadRequest)
  ::PROTOBUF_NAMESPACE_ID::uint32 cached_has_bits = 0;
  (void) cached_has_bits;

  // string client_ip = 1;
  if (this->client_ip().size() > 0) {
    ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::VerifyUtf8String(
      this->_internal_client_ip().data(), static_cast<int>(this->_internal_client_ip().length()),
      ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::SERIALIZE,
      "proto.ClientReadRequest.client_ip");
    target = stream->WriteStringMaybeAliased(
        1, this->_internal_client_ip(), target);
  }

  // uint64 txn_id = 2;
  if (this->txn_id() != 0) {
    target = stream->EnsureSpace(target);
    target = ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::WriteUInt64ToArray(2, this->_internal_txn_id(), target);
  }

  // repeated .proto.Row rows = 3;
  for (unsigned int i = 0,
      n = static_cast<unsigned int>(this->_internal_rows_size()); i < n; i++) {
    target = stream->EnsureSpace(target);
    target = ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::
      InternalWriteMessage(3, this->_internal_rows(i), target, stream);
  }

  if (PROTOBUF_PREDICT_FALSE(_internal_metadata_.have_unknown_fields())) {
    target = ::PROTOBUF_NAMESPACE_ID::internal::WireFormat::InternalSerializeUnknownFieldsToArray(
        _internal_metadata_.unknown_fields(), target, stream);
  }
  // @@protoc_insertion_point(serialize_to_array_end:proto.ClientReadRequest)
  return target;
}

size_t ClientReadRequest::ByteSizeLong() const {
// @@protoc_insertion_point(message_byte_size_start:proto.ClientReadRequest)
  size_t total_size = 0;

  ::PROTOBUF_NAMESPACE_ID::uint32 cached_has_bits = 0;
  // Prevent compiler warnings about cached_has_bits being unused
  (void) cached_has_bits;

  // repeated .proto.Row rows = 3;
  total_size += 1UL * this->_internal_rows_size();
  for (const auto& msg : this->rows_) {
    total_size +=
      ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::MessageSize(msg);
  }

  // string client_ip = 1;
  if (this->client_ip().size() > 0) {
    total_size += 1 +
      ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::StringSize(
        this->_internal_client_ip());
  }

  // uint64 txn_id = 2;
  if (this->txn_id() != 0) {
    total_size += 1 +
      ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::UInt64Size(
        this->_internal_txn_id());
  }

  if (PROTOBUF_PREDICT_FALSE(_internal_metadata_.have_unknown_fields())) {
    return ::PROTOBUF_NAMESPACE_ID::internal::ComputeUnknownFieldsSize(
        _internal_metadata_, total_size, &_cached_size_);
  }
  int cached_size = ::PROTOBUF_NAMESPACE_ID::internal::ToCachedSize(total_size);
  SetCachedSize(cached_size);
  return total_size;
}

void ClientReadRequest::MergeFrom(const ::PROTOBUF_NAMESPACE_ID::Message& from) {
// @@protoc_insertion_point(generalized_merge_from_start:proto.ClientReadRequest)
  GOOGLE_DCHECK_NE(&from, this);
  const ClientReadRequest* source =
      ::PROTOBUF_NAMESPACE_ID::DynamicCastToGenerated<ClientReadRequest>(
          &from);
  if (source == nullptr) {
  // @@protoc_insertion_point(generalized_merge_from_cast_fail:proto.ClientReadRequest)
    ::PROTOBUF_NAMESPACE_ID::internal::ReflectionOps::Merge(from, this);
  } else {
  // @@protoc_insertion_point(generalized_merge_from_cast_success:proto.ClientReadRequest)
    MergeFrom(*source);
  }
}

void ClientReadRequest::MergeFrom(const ClientReadRequest& from) {
// @@protoc_insertion_point(class_specific_merge_from_start:proto.ClientReadRequest)
  GOOGLE_DCHECK_NE(&from, this);
  _internal_metadata_.MergeFrom(from._internal_metadata_);
  ::PROTOBUF_NAMESPACE_ID::uint32 cached_has_bits = 0;
  (void) cached_has_bits;

  rows_.MergeFrom(from.rows_);
  if (from.client_ip().size() > 0) {

    client_ip_.AssignWithDefault(&::PROTOBUF_NAMESPACE_ID::internal::GetEmptyStringAlreadyInited(), from.client_ip_);
  }
  if (from.txn_id() != 0) {
    _internal_set_txn_id(from._internal_txn_id());
  }
}

void ClientReadRequest::CopyFrom(const ::PROTOBUF_NAMESPACE_ID::Message& from) {
// @@protoc_insertion_point(generalized_copy_from_start:proto.ClientReadRequest)
  if (&from == this) return;
  Clear();
  MergeFrom(from);
}

void ClientReadRequest::CopyFrom(const ClientReadRequest& from) {
// @@protoc_insertion_point(class_specific_copy_from_start:proto.ClientReadRequest)
  if (&from == this) return;
  Clear();
  MergeFrom(from);
}

bool ClientReadRequest::IsInitialized() const {
  return true;
}

void ClientReadRequest::InternalSwap(ClientReadRequest* other) {
  using std::swap;
  _internal_metadata_.Swap(&other->_internal_metadata_);
  rows_.InternalSwap(&other->rows_);
  client_ip_.Swap(&other->client_ip_, &::PROTOBUF_NAMESPACE_ID::internal::GetEmptyStringAlreadyInited(),
    GetArenaNoVirtual());
  swap(txn_id_, other->txn_id_);
}

::PROTOBUF_NAMESPACE_ID::Metadata ClientReadRequest::GetMetadata() const {
  return GetMetadataStatic();
}


// ===================================================================

void ClientReadResponse::InitAsDefaultInstance() {
}
class ClientReadResponse::_Internal {
 public:
};

void ClientReadResponse::clear_rows() {
  rows_.Clear();
}
ClientReadResponse::ClientReadResponse()
  : ::PROTOBUF_NAMESPACE_ID::Message(), _internal_metadata_(nullptr) {
  SharedCtor();
  // @@protoc_insertion_point(constructor:proto.ClientReadResponse)
}
ClientReadResponse::ClientReadResponse(const ClientReadResponse& from)
  : ::PROTOBUF_NAMESPACE_ID::Message(),
      _internal_metadata_(nullptr),
      rows_(from.rows_) {
  _internal_metadata_.MergeFrom(from._internal_metadata_);
  ::memcpy(&txn_id_, &from.txn_id_,
    static_cast<size_t>(reinterpret_cast<char*>(&result_) -
    reinterpret_cast<char*>(&txn_id_)) + sizeof(result_));
  // @@protoc_insertion_point(copy_constructor:proto.ClientReadResponse)
}

void ClientReadResponse::SharedCtor() {
  ::PROTOBUF_NAMESPACE_ID::internal::InitSCC(&scc_info_ClientReadResponse_client_2eproto.base);
  ::memset(&txn_id_, 0, static_cast<size_t>(
      reinterpret_cast<char*>(&result_) -
      reinterpret_cast<char*>(&txn_id_)) + sizeof(result_));
}

ClientReadResponse::~ClientReadResponse() {
  // @@protoc_insertion_point(destructor:proto.ClientReadResponse)
  SharedDtor();
}

void ClientReadResponse::SharedDtor() {
}

void ClientReadResponse::SetCachedSize(int size) const {
  _cached_size_.Set(size);
}
const ClientReadResponse& ClientReadResponse::default_instance() {
  ::PROTOBUF_NAMESPACE_ID::internal::InitSCC(&::scc_info_ClientReadResponse_client_2eproto.base);
  return *internal_default_instance();
}


void ClientReadResponse::Clear() {
// @@protoc_insertion_point(message_clear_start:proto.ClientReadResponse)
  ::PROTOBUF_NAMESPACE_ID::uint32 cached_has_bits = 0;
  // Prevent compiler warnings about cached_has_bits being unused
  (void) cached_has_bits;

  rows_.Clear();
  ::memset(&txn_id_, 0, static_cast<size_t>(
      reinterpret_cast<char*>(&result_) -
      reinterpret_cast<char*>(&txn_id_)) + sizeof(result_));
  _internal_metadata_.Clear();
}

const char* ClientReadResponse::_InternalParse(const char* ptr, ::PROTOBUF_NAMESPACE_ID::internal::ParseContext* ctx) {
#define CHK_(x) if (PROTOBUF_PREDICT_FALSE(!(x))) goto failure
  while (!ctx->Done(&ptr)) {
    ::PROTOBUF_NAMESPACE_ID::uint32 tag;
    ptr = ::PROTOBUF_NAMESPACE_ID::internal::ReadTag(ptr, &tag);
    CHK_(ptr);
    switch (tag >> 3) {
      // .proto.Result result = 1;
      case 1:
        if (PROTOBUF_PREDICT_TRUE(static_cast<::PROTOBUF_NAMESPACE_ID::uint8>(tag) == 8)) {
          ::PROTOBUF_NAMESPACE_ID::uint64 val = ::PROTOBUF_NAMESPACE_ID::internal::ReadVarint(&ptr);
          CHK_(ptr);
          _internal_set_result(static_cast<::proto::Result>(val));
        } else goto handle_unusual;
        continue;
      // uint64 txn_id = 2;
      case 2:
        if (PROTOBUF_PREDICT_TRUE(static_cast<::PROTOBUF_NAMESPACE_ID::uint8>(tag) == 16)) {
          txn_id_ = ::PROTOBUF_NAMESPACE_ID::internal::ReadVarint(&ptr);
          CHK_(ptr);
        } else goto handle_unusual;
        continue;
      // repeated .proto.Row rows = 3;
      case 3:
        if (PROTOBUF_PREDICT_TRUE(static_cast<::PROTOBUF_NAMESPACE_ID::uint8>(tag) == 26)) {
          ptr -= 1;
          do {
            ptr += 1;
            ptr = ctx->ParseMessage(_internal_add_rows(), ptr);
            CHK_(ptr);
            if (!ctx->DataAvailable(ptr)) break;
          } while (::PROTOBUF_NAMESPACE_ID::internal::ExpectTag<26>(ptr));
        } else goto handle_unusual;
        continue;
      default: {
      handle_unusual:
        if ((tag & 7) == 4 || tag == 0) {
          ctx->SetLastTag(tag);
          goto success;
        }
        ptr = UnknownFieldParse(tag, &_internal_metadata_, ptr, ctx);
        CHK_(ptr != nullptr);
        continue;
      }
    }  // switch
  }  // while
success:
  return ptr;
failure:
  ptr = nullptr;
  goto success;
#undef CHK_
}

::PROTOBUF_NAMESPACE_ID::uint8* ClientReadResponse::_InternalSerialize(
    ::PROTOBUF_NAMESPACE_ID::uint8* target, ::PROTOBUF_NAMESPACE_ID::io::EpsCopyOutputStream* stream) const {
  // @@protoc_insertion_point(serialize_to_array_start:proto.ClientReadResponse)
  ::PROTOBUF_NAMESPACE_ID::uint32 cached_has_bits = 0;
  (void) cached_has_bits;

  // .proto.Result result = 1;
  if (this->result() != 0) {
    target = stream->EnsureSpace(target);
    target = ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::WriteEnumToArray(
      1, this->_internal_result(), target);
  }

  // uint64 txn_id = 2;
  if (this->txn_id() != 0) {
    target = stream->EnsureSpace(target);
    target = ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::WriteUInt64ToArray(2, this->_internal_txn_id(), target);
  }

  // repeated .proto.Row rows = 3;
  for (unsigned int i = 0,
      n = static_cast<unsigned int>(this->_internal_rows_size()); i < n; i++) {
    target = stream->EnsureSpace(target);
    target = ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::
      InternalWriteMessage(3, this->_internal_rows(i), target, stream);
  }

  if (PROTOBUF_PREDICT_FALSE(_internal_metadata_.have_unknown_fields())) {
    target = ::PROTOBUF_NAMESPACE_ID::internal::WireFormat::InternalSerializeUnknownFieldsToArray(
        _internal_metadata_.unknown_fields(), target, stream);
  }
  // @@protoc_insertion_point(serialize_to_array_end:proto.ClientReadResponse)
  return target;
}

size_t ClientReadResponse::ByteSizeLong() const {
// @@protoc_insertion_point(message_byte_size_start:proto.ClientReadResponse)
  size_t total_size = 0;

  ::PROTOBUF_NAMESPACE_ID::uint32 cached_has_bits = 0;
  // Prevent compiler warnings about cached_has_bits being unused
  (void) cached_has_bits;

  // repeated .proto.Row rows = 3;
  total_size += 1UL * this->_internal_rows_size();
  for (const auto& msg : this->rows_) {
    total_size +=
      ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::MessageSize(msg);
  }

  // uint64 txn_id = 2;
  if (this->txn_id() != 0) {
    total_size += 1 +
      ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::UInt64Size(
        this->_internal_txn_id());
  }

  // .proto.Result result = 1;
  if (this->result() != 0) {
    total_size += 1 +
      ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::EnumSize(this->_internal_result());
  }

  if (PROTOBUF_PREDICT_FALSE(_internal_metadata_.have_unknown_fields())) {
    return ::PROTOBUF_NAMESPACE_ID::internal::ComputeUnknownFieldsSize(
        _internal_metadata_, total_size, &_cached_size_);
  }
  int cached_size = ::PROTOBUF_NAMESPACE_ID::internal::ToCachedSize(total_size);
  SetCachedSize(cached_size);
  return total_size;
}

void ClientReadResponse::MergeFrom(const ::PROTOBUF_NAMESPACE_ID::Message& from) {
// @@protoc_insertion_point(generalized_merge_from_start:proto.ClientReadResponse)
  GOOGLE_DCHECK_NE(&from, this);
  const ClientReadResponse* source =
      ::PROTOBUF_NAMESPACE_ID::DynamicCastToGenerated<ClientReadResponse>(
          &from);
  if (source == nullptr) {
  // @@protoc_insertion_point(generalized_merge_from_cast_fail:proto.ClientReadResponse)
    ::PROTOBUF_NAMESPACE_ID::internal::ReflectionOps::Merge(from, this);
  } else {
  // @@protoc_insertion_point(generalized_merge_from_cast_success:proto.ClientReadResponse)
    MergeFrom(*source);
  }
}

void ClientReadResponse::MergeFrom(const ClientReadResponse& from) {
// @@protoc_insertion_point(class_specific_merge_from_start:proto.ClientReadResponse)
  GOOGLE_DCHECK_NE(&from, this);
  _internal_metadata_.MergeFrom(from._internal_metadata_);
  ::PROTOBUF_NAMESPACE_ID::uint32 cached_has_bits = 0;
  (void) cached_has_bits;

  rows_.MergeFrom(from.rows_);
  if (from.txn_id() != 0) {
    _internal_set_txn_id(from._internal_txn_id());
  }
  if (from.result() != 0) {
    _internal_set_result(from._internal_result());
  }
}

void ClientReadResponse::CopyFrom(const ::PROTOBUF_NAMESPACE_ID::Message& from) {
// @@protoc_insertion_point(generalized_copy_from_start:proto.ClientReadResponse)
  if (&from == this) return;
  Clear();
  MergeFrom(from);
}

void ClientReadResponse::CopyFrom(const ClientReadResponse& from) {
// @@protoc_insertion_point(class_specific_copy_from_start:proto.ClientReadResponse)
  if (&from == this) return;
  Clear();
  MergeFrom(from);
}

bool ClientReadResponse::IsInitialized() const {
  return true;
}

void ClientReadResponse::InternalSwap(ClientReadResponse* other) {
  using std::swap;
  _internal_metadata_.Swap(&other->_internal_metadata_);
  rows_.InternalSwap(&other->rows_);
  swap(txn_id_, other->txn_id_);
  swap(result_, other->result_);
}

::PROTOBUF_NAMESPACE_ID::Metadata ClientReadResponse::GetMetadata() const {
  return GetMetadataStatic();
}


// @@protoc_insertion_point(namespace_scope)
}  // namespace proto
PROTOBUF_NAMESPACE_OPEN
template<> PROTOBUF_NOINLINE ::proto::ClientReadRequest* Arena::CreateMaybeMessage< ::proto::ClientReadRequest >(Arena* arena) {
  return Arena::CreateInternal< ::proto::ClientReadRequest >(arena);
}
template<> PROTOBUF_NOINLINE ::proto::ClientReadResponse* Arena::CreateMaybeMessage< ::proto::ClientReadResponse >(Arena* arena) {
  return Arena::CreateInternal< ::proto::ClientReadResponse >(arena);
}
PROTOBUF_NAMESPACE_CLOSE

// @@protoc_insertion_point(global_scope)
#include <google/protobuf/port_undef.inc>
