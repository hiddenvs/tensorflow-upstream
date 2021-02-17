/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/xla/service/spmd/schedule_aware_all_gather_cse.h"

#include "tensorflow/compiler/xla/service/hlo_matchers.h"
#include "tensorflow/compiler/xla/service/hlo_opcode.h"
#include "tensorflow/compiler/xla/service/hlo_parser.h"
#include "tensorflow/compiler/xla/service/hlo_pass_pipeline.h"
#include "tensorflow/compiler/xla/service/hlo_verifier.h"
#include "tensorflow/compiler/xla/tests/hlo_test_base.h"
#include "tensorflow/compiler/xla/xla_data.pb.h"
#include "tensorflow/core/lib/core/status_test_util.h"

namespace xla {
namespace spmd {
namespace {

class AllGatherCseTest : public HloTestBase {
 public:
  StatusOr<std::unique_ptr<HloModule>> RunPass(absl::string_view hlo_module,
                                               int64 distance_threshold = 100) {
    TF_ASSIGN_OR_RETURN(auto module, ParseAndReturnVerifiedModule(
                                         hlo_module, GetModuleConfigForTest()));
    HloPassPipeline pipeline("all-gather-cse");
    pipeline.AddPass<ScheduleAwareAllGatherCSE>(distance_threshold,
                                                /*for_replicas=*/false);
    TF_RETURN_IF_ERROR(pipeline.Run(module.get()).status());
    return StatusOr<std::unique_ptr<HloModule>>(std::move(module));
  }
};

TEST_F(AllGatherCseTest, SimpleCse) {
  absl::string_view hlo_string = R"(
HloModule module

ENTRY entry {
  param0 = s32[1,8]{1,0} parameter(0)
  ag1 = s32[2,8]{1,0} all-gather(param0), replica_groups={{0,1}}, dimensions={0},
    channel_id=0, use_global_device_ids=true
  ag2 = s32[2,8]{1,0} all-gather(param0), replica_groups={{0,1}}, dimensions={0},
    channel_id=1, use_global_device_ids=true
  ROOT tuple = (s32[2,8]{1,0}, s32[2,8]{1,0}) tuple(ag1, ag2)
})";
  auto module_status = RunPass(hlo_string);
  EXPECT_TRUE(module_status.status().ok());
  auto module = module_status.ConsumeValueOrDie();
  HloInstruction* tuple = module->entry_computation()->root_instruction();
  EXPECT_EQ(tuple->opcode(), HloOpcode::kTuple);
  EXPECT_EQ(tuple->operand_count(), 2);
  EXPECT_EQ(tuple->operand(0), tuple->operand(1));
}

TEST_F(AllGatherCseTest, SimpleCseDifferentDim) {
  absl::string_view hlo_string = R"(
HloModule module

ENTRY entry {
  param0 = s32[1,8]{1,0} parameter(0)
  ag1 = s32[1,16]{1,0} all-gather(param0), replica_groups={{0,1}}, dimensions={1},
    channel_id=0, use_global_device_ids=true
  ag2 = s32[1,16]{1,0} all-gather(param0), replica_groups={{0,1}},
    dimensions={1}, channel_id=1, use_global_device_ids=true
  ROOT tuple = (s32[1,16]{1,0}, s32[2,8,1,1]{3,2,1,0}) tuple(ag1, ag2)
})";
  auto module_status = RunPass(hlo_string);
  EXPECT_TRUE(module_status.status().ok());
  auto module = module_status.ConsumeValueOrDie();
  HloInstruction* tuple = module->entry_computation()->root_instruction();
  EXPECT_EQ(tuple->opcode(), HloOpcode::kTuple);
  EXPECT_EQ(tuple->operand_count(), 2);
  EXPECT_EQ(tuple->operand(0), tuple->operand(1));
}

TEST_F(AllGatherCseTest, NoCseGlobalDevice) {
  absl::string_view hlo_string = R"(
HloModule module

ENTRY entry {
  param0 = s32[1,8]{1,0} parameter(0)
  ag1 = s32[2,8]{1,0} all-gather(param0), replica_groups={{0,1}}, dimensions={0},
    channel_id=0, use_global_device_ids=true
  ag2 = s32[2,8]{1,0} all-gather(param0), replica_groups={{0},{1}}, dimensions={0},
    channel_id=1, use_global_device_ids=false
  ROOT tuple = (s32[2,8]{1,0}, s32[2,8]{1,0}) tuple(ag1, ag2)
})";
  auto module_status = RunPass(hlo_string);
  EXPECT_TRUE(module_status.status().ok());
  auto module = module_status.ConsumeValueOrDie();
  HloInstruction* tuple = module->entry_computation()->root_instruction();
  EXPECT_EQ(tuple->opcode(), HloOpcode::kTuple);
  EXPECT_EQ(tuple->operand_count(), 2);
  EXPECT_NE(tuple->operand(0), tuple->operand(1));
}

TEST_F(AllGatherCseTest, NoCseChannelIdMismatch) {
  absl::string_view hlo_string = R"(
HloModule module

ENTRY entry {
  param0 = s32[1,8]{1,0} parameter(0)
  ag1 = s32[1,16]{1,0} all-gather(param0), replica_groups={{0,1}}, dimensions={1},
    channel_id=0, use_global_device_ids=true
  ag2 = s32[1,16]{1,0} all-gather(param0), replica_groups={{0,1}},
    dimensions={1}, use_global_device_ids=true
  ROOT tuple = (s32[1,16]{1,0}, s32[1,16]{1,0}) tuple(ag1, ag2)
})";
  auto module_status = RunPass(hlo_string);
  EXPECT_TRUE(module_status.status().ok());
  auto module = module_status.ConsumeValueOrDie();
  HloInstruction* tuple = module->entry_computation()->root_instruction();
  EXPECT_EQ(tuple->opcode(), HloOpcode::kTuple);
  EXPECT_EQ(tuple->operand_count(), 2);
  EXPECT_NE(tuple->operand(0), tuple->operand(1));
}

}  // namespace
}  // namespace spmd
}  // namespace xla
