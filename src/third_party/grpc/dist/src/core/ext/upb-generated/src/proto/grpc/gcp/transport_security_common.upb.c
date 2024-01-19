/* This file was generated by upbc (the upb compiler) from the input
 * file:
 *
 *     src/proto/grpc/gcp/transport_security_common.proto
 *
 * Do not edit -- your changes will be discarded when the file is
 * regenerated. */

#include <stddef.h>
#include "upb/generated_code_support.h"
#include "src/proto/grpc/gcp/transport_security_common.upb.h"

// Must be last.
#include "upb/port/def.inc"

static const upb_MiniTableSub grpc_gcp_RpcProtocolVersions_submsgs[2] = {
  {.submsg = &grpc_gcp_RpcProtocolVersions_Version_msg_init},
  {.submsg = &grpc_gcp_RpcProtocolVersions_Version_msg_init},
};

static const upb_MiniTableField grpc_gcp_RpcProtocolVersions__fields[2] = {
  {1, UPB_SIZE(4, 8), 1, 0, 11, (int)kUpb_FieldMode_Scalar | ((int)UPB_SIZE(kUpb_FieldRep_4Byte, kUpb_FieldRep_8Byte) << kUpb_FieldRep_Shift)},
  {2, UPB_SIZE(8, 16), 2, 1, 11, (int)kUpb_FieldMode_Scalar | ((int)UPB_SIZE(kUpb_FieldRep_4Byte, kUpb_FieldRep_8Byte) << kUpb_FieldRep_Shift)},
};

const upb_MiniTable grpc_gcp_RpcProtocolVersions_msg_init = {
  &grpc_gcp_RpcProtocolVersions_submsgs[0],
  &grpc_gcp_RpcProtocolVersions__fields[0],
  UPB_SIZE(16, 24), 2, kUpb_ExtMode_NonExtendable, 2, UPB_FASTTABLE_MASK(24), 0,
  UPB_FASTTABLE_INIT({
    {0x0000000000000000, &_upb_FastDecoder_DecodeGeneric},
    {0x000800000100000a, &upb_psm_1bt_max64b},
    {0x0010000002010012, &upb_psm_1bt_max64b},
    {0x0000000000000000, &_upb_FastDecoder_DecodeGeneric},
  })
};

static const upb_MiniTableField grpc_gcp_RpcProtocolVersions_Version__fields[2] = {
  {1, 0, 0, kUpb_NoSub, 13, (int)kUpb_FieldMode_Scalar | ((int)kUpb_FieldRep_4Byte << kUpb_FieldRep_Shift)},
  {2, 4, 0, kUpb_NoSub, 13, (int)kUpb_FieldMode_Scalar | ((int)kUpb_FieldRep_4Byte << kUpb_FieldRep_Shift)},
};

const upb_MiniTable grpc_gcp_RpcProtocolVersions_Version_msg_init = {
  NULL,
  &grpc_gcp_RpcProtocolVersions_Version__fields[0],
  8, 2, kUpb_ExtMode_NonExtendable, 2, UPB_FASTTABLE_MASK(24), 0,
  UPB_FASTTABLE_INIT({
    {0x0000000000000000, &_upb_FastDecoder_DecodeGeneric},
    {0x000000003f000008, &upb_psv4_1bt},
    {0x000400003f000010, &upb_psv4_1bt},
    {0x0000000000000000, &_upb_FastDecoder_DecodeGeneric},
  })
};

static const upb_MiniTable *messages_layout[2] = {
  &grpc_gcp_RpcProtocolVersions_msg_init,
  &grpc_gcp_RpcProtocolVersions_Version_msg_init,
};

const upb_MiniTableFile src_proto_grpc_gcp_transport_security_common_proto_upb_file_layout = {
  messages_layout,
  NULL,
  NULL,
  2,
  0,
  0,
};

#include "upb/port/undef.inc"
