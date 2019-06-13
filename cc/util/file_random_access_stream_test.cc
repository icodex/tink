// Copyright 2019 Google Inc.
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
//
///////////////////////////////////////////////////////////////////////////////

#include "tink/util/file_random_access_stream.h"

#include <fcntl.h>
#include <unistd.h>
#include <thread>  // NOLINT(build/c++11)

#include "gtest/gtest.h"
#include "absl/memory/memory.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "tink/subtle/random.h"
#include "tink/util/buffer.h"
#include "tink/util/test_util.h"

namespace crypto {
namespace tink {
namespace util {
namespace {

// Creates a new test file with the specified 'filename', writes 'size' random
// bytes to the file, and returns a file descriptor for reading from the file.
// A copy of the bytes written to the file is returned in 'file_contents'.
int GetTestFileDescriptor(
    absl::string_view filename, int size, std::string* file_contents) {
  std::string full_filename =
      absl::StrCat(crypto::tink::test::TmpDir(), "/", filename);
  (*file_contents) = subtle::Random::GetRandomBytes(size);
  mode_t mode = S_IWUSR | S_IRUSR | S_IRGRP | S_IROTH;
  int fd = open(full_filename.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
  if (fd == -1) {
    std::clog << "Cannot create file " << full_filename
              << " error: " << errno << std::endl;
    exit(1);
  }
  if (write(fd, file_contents->data(), size) != size) {
    std::clog << "Failed to write " << size << " bytes to file "
              << full_filename << " error: " << errno << std::endl;

    exit(1);
  }
  close(fd);
  fd = open(full_filename.c_str(), O_RDONLY);
  if (fd == -1) {
    std::clog << "Cannot re-open file " << full_filename
              << " error: " << errno << std::endl;
    exit(1);
  }
  return fd;
}

// Reads the entire 'ra_stream' in chunks of size 'chunk_size',
// until no more bytes can be read, and puts the read bytes into 'contents'.
// Returns the status of the last ra_stream->Next()-operation.
util::Status ReadAll(RandomAccessStream* ra_stream, int chunk_size,
                     std::string* contents) {
  contents->clear();
  auto buffer = std::move(Buffer::New(chunk_size).ValueOrDie());
  int64_t position = 0;
  auto status = ra_stream->PRead(position, chunk_size, buffer.get());
  while (status.ok()) {
    contents->append(buffer->get_mem_block(), buffer->size());
    position = contents->size();
    status = ra_stream->PRead(position, chunk_size, buffer.get());
  }
  if (status.error_code() == util::error::OUT_OF_RANGE) {  // EOF
    EXPECT_EQ(0, buffer->size());
  }
  return status;
}

// Reads from 'ra_stream' a chunk of 'count' bytes starting offset 'position',
// and compares the read bytes to the corresponding bytes in 'file_contents'.
void ReadAndVerifyChunk(RandomAccessStream* ra_stream,
                        int64_t position,
                        int count,
                        absl::string_view file_contents) {
  SCOPED_TRACE(absl::StrCat("stream_size = ", file_contents.size(),
                            ", position = ", position,
                            ", count = ", count));
  auto buffer = std::move(Buffer::New(count).ValueOrDie());
  int stream_size = ra_stream->size();
  EXPECT_EQ(file_contents.size(), stream_size);
  auto status = ra_stream->PRead(position, count, buffer.get());
  EXPECT_TRUE(status.ok());
  int read_count = buffer->size();
  int expected_count = count;
  if (position + count > stream_size) {
    expected_count = stream_size - position;
  }
  EXPECT_EQ(expected_count, read_count);
  EXPECT_EQ(0, memcmp(file_contents.substr(position, read_count).data(),
                      buffer->get_mem_block(), read_count));
}

TEST(FileRandomAccessStreamTest, ReadingStreams) {
  //  for (auto stream_size : {0, 10, 100, 1000, 10000, 1000000}) {
  for (auto stream_size : {0, 10, 100, 1000}) {
    SCOPED_TRACE(absl::StrCat("stream_size = ", stream_size));
    std::string file_contents;
    std::string filename = absl::StrCat(stream_size, "_reading_test.bin");
    int input_fd = GetTestFileDescriptor(filename, stream_size, &file_contents);
    EXPECT_EQ(stream_size, file_contents.size());
    auto ra_stream = absl::make_unique<util::FileRandomAccessStream>(input_fd);
    std::string stream_contents;
    auto status = ReadAll(ra_stream.get(), 1 + (stream_size / 10),
                          &stream_contents);
    EXPECT_EQ(util::error::OUT_OF_RANGE, status.error_code());
    EXPECT_EQ("EOF", status.error_message());
    EXPECT_EQ(file_contents, stream_contents);
    EXPECT_EQ(stream_size, ra_stream->size());
  }
}


TEST(FileRandomAccessStreamTest, ConcurrentReads) {
  for (auto stream_size : {100, 1000, 10000, 100000}) {
    std::string file_contents;
    std::string filename = absl::StrCat(stream_size, "_reading_test.bin");
    int input_fd = GetTestFileDescriptor(filename, stream_size, &file_contents);
    EXPECT_EQ(stream_size, file_contents.size());
    auto ra_stream = absl::make_unique<util::FileRandomAccessStream>(input_fd);
    std::thread read_0(ReadAndVerifyChunk,
        ra_stream.get(), 0, stream_size / 2, file_contents);
    std::thread read_1(ReadAndVerifyChunk,
        ra_stream.get(), stream_size / 4, stream_size / 2, file_contents);
    std::thread read_2(ReadAndVerifyChunk,
        ra_stream.get(), stream_size / 2, stream_size / 2, file_contents);
    std::thread read_3(ReadAndVerifyChunk,
        ra_stream.get(), 3 * stream_size / 4, stream_size / 2, file_contents);
    read_0.join();
    read_1.join();
    read_2.join();
    read_3.join();
  }
}

TEST(FileRandomAccessStreamTest, NegativeReadPosition) {
  for (auto stream_size : {0, 10, 100, 1000, 10000}) {
    std::string file_contents;
    std::string filename = absl::StrCat(stream_size, "_reading_test.bin");
    int input_fd = GetTestFileDescriptor(filename, stream_size, &file_contents);
    auto ra_stream = absl::make_unique<util::FileRandomAccessStream>(input_fd);
    int count = 42;
    auto buffer = std::move(Buffer::New(count).ValueOrDie());
    for (auto position : {-100, -10, -1}) {
      SCOPED_TRACE(absl::StrCat("stream_size = ", stream_size,
                                " position = ", position));

      auto status = ra_stream->PRead(position, count, buffer.get());
      EXPECT_EQ(util::error::INVALID_ARGUMENT, status.error_code());
    }
  }
}

TEST(FileRandomAccessStreamTest, NegativeReadCount) {
  for (auto stream_size : {0, 10, 100, 1000, 10000}) {
    std::string file_contents;
    std::string filename = absl::StrCat(stream_size, "_reading_test.bin");
    int input_fd = GetTestFileDescriptor(filename, stream_size, &file_contents);
    auto ra_stream = absl::make_unique<util::FileRandomAccessStream>(input_fd);
    auto buffer = std::move(Buffer::New(42).ValueOrDie());
    int64_t position = 0;
    for (auto count : {-100, -10, -1}) {
      SCOPED_TRACE(absl::StrCat("stream_size = ", stream_size,
                                " count = ", count));
      auto status = ra_stream->PRead(position, count, buffer.get());
      EXPECT_EQ(util::error::INVALID_ARGUMENT, status.error_code());
    }
  }
}

TEST(FileRandomAccessStreamTest, ReadPositionAfterEof) {
  for (auto stream_size : {0, 10, 100, 1000, 10000}) {
    std::string file_contents;
    std::string filename = absl::StrCat(stream_size, "_reading_test.bin");
    int input_fd = GetTestFileDescriptor(filename, stream_size, &file_contents);
    auto ra_stream = absl::make_unique<util::FileRandomAccessStream>(input_fd);
    int count = 42;
    auto buffer = std::move(Buffer::New(count).ValueOrDie());
    for (auto position : {stream_size + 1, stream_size + 10}) {
      SCOPED_TRACE(absl::StrCat("stream_size = ", stream_size,
                                " position = ", position));

      auto status = ra_stream->PRead(position, count, buffer.get());
      EXPECT_EQ(util::error::OUT_OF_RANGE, status.error_code());
      EXPECT_EQ(0, buffer->size());
    }
  }
}

}  // namespace
}  // namespace util
}  // namespace tink
}  // namespace crypto
