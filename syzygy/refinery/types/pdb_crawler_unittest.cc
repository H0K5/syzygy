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

#include "syzygy/refinery/types/pdb_crawler.h"

#include "base/path_service.h"
#include "base/debug/alias.h"
#include "base/files/file_path.h"
#include "base/strings/string_util.h"
#include "gtest/gtest.h"
#include "syzygy/core/unittest_util.h"
#include "syzygy/refinery/types/type.h"
#include "syzygy/refinery/types/type_repository.h"

namespace refinery {

namespace {

class PdbCrawlerTest : public testing::Test {
 protected:
};

}  // namespace

TEST_F(PdbCrawlerTest, InitializeForFile) {
  PdbCrawler crawler;

  ASSERT_TRUE(crawler.InitializeForFile(testing::GetSrcRelativePath(
      L"syzygy\\refinery\\test_data\\test_types.dll.pdb")));

  TypeRepository types;
  ASSERT_TRUE(crawler.GetTypes(&types));

  ASSERT_LE(1U, types.size());

  // This should find the member 'three' of TestSimpleUDT.
  TypePtr type;
  for (auto it = types.begin(); it != types.end(); ++it) {
    if (EndsWith((*it)->name(), L" const* volatile*", true)) {
      type = *it;
      break;
    }
  }

  ASSERT_NE(nullptr, type);

  EXPECT_EQ(Type::POINTER_TYPE_KIND, type->kind());
  EXPECT_EQ(4, type->size());

  PointerTypePtr ptr;
  ASSERT_TRUE(type->CastTo(&ptr));
  ASSERT_TRUE(ptr);
  EXPECT_EQ(4, ptr->size());
  EXPECT_FALSE(ptr->is_const());
  EXPECT_TRUE(ptr->is_volatile());
  ASSERT_TRUE(ptr->GetContentType());
  EXPECT_EQ(Type::POINTER_TYPE_KIND, ptr->GetContentType()->kind());

  // The basic types names will be eventually different.
  EXPECT_EQ(L"T_SHORT const* volatile*", ptr->name());

  ASSERT_TRUE(ptr->GetContentType()->CastTo(&ptr));
  ASSERT_TRUE(ptr);
  EXPECT_EQ(4, ptr->size());
  EXPECT_TRUE(ptr->is_const());
  EXPECT_FALSE(ptr->is_volatile());
  ASSERT_TRUE(ptr->GetContentType());
  EXPECT_EQ(L"T_SHORT const*", ptr->name());

  EXPECT_EQ(Type::BASIC_TYPE_KIND, ptr->GetContentType()->kind());
  EXPECT_EQ(L"T_SHORT", ptr->GetContentType()->name());
}

}  // namespace refinery