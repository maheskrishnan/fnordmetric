/**
 * This file is part of the "FnordMetric" project
 *   Copyright (c) 2014 Paul Asmuth, Google Inc.
 *
 * Licensed under the MIT license (see LICENSE).
 */
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <algorithm>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/fcntl.h>
#include <unistd.h>
#include <assert.h>
#include "filebackend.h"
#include "cursor.h"
#include "streamref.h"
#include "pagemanager.h"
#include "../clock.h"

namespace fnordmetric {
namespace filebackend {

FileBackend::FileBackend(
    PageManager&& page_manager,
    MmapPageManager&& mmap_manager) :
    page_manager_(std::move(page_manager)),
    mmap_manager_(std::move(mmap_manager)),
    max_stream_id_(0),
    IStorageBackend() {}

std::unique_ptr<FileBackend> FileBackend::openFile(const std::string& filename) {
  FileBackend* ptr = nullptr;

  int fd = open(filename.c_str(), O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
  if (fd < 0) {
    perror("open() failed");
    return std::unique_ptr<FileBackend>(nullptr);
  }

  struct stat fd_stat;
  if (fstat(fd, &fd_stat) < 0) {
    perror("fstat() failed");
    return std::unique_ptr<FileBackend>(nullptr);
  }

  off_t fd_len = lseek(fd, 0, SEEK_END);
  if (fd_len < 0) {
    perror("lseek() failed");
    return std::unique_ptr<FileBackend>(nullptr);
  }

  MmapPageManager mmap_manager(fd, fd_len);

  // FIXPAUL last used page should be from log
  size_t last_used_byte = 0;

  auto backend = new FileBackend(
      PageManager(last_used_byte, fd_stat.st_blksize),
      std::move(mmap_manager));

  return std::unique_ptr<FileBackend>(backend);
}

std::unique_ptr<IStorageCursor> FileBackend::getCursor(const std::string& key) {
  return std::unique_ptr<Cursor>(new Cursor(getStreamRef(key)));
}

// FIXPAUL locking!
std::shared_ptr<StreamRef> FileBackend::getStreamRef(const std::string& key) {
  auto stream_id = getStreamId(key);
  auto iter = stream_refs_.find(stream_id);

  if (iter == stream_refs_.end()) {
    std::shared_ptr<StreamRef> stream_ref(new StreamRef(this, stream_id, key));
    stream_refs_.insert(std::make_pair(stream_id, stream_ref));
    return stream_ref;
  } else {
    return iter->second;
  }
}

// FIXPAUL must hold lock to call!
uint64_t FileBackend::getStreamId(const std::string& stream_key) {
  auto iter = stream_ids_.find(stream_key);

  if (iter == stream_ids_.end()) {
    auto stream_id = ++max_stream_id_;
    stream_ids_.insert(std::make_pair(stream_key, stream_id));
    return stream_id;
  } else {
    return iter->second;
  }
}

Cursor::Cursor(std::shared_ptr<StreamRef> stream_ref) :
    stream_ref_(stream_ref) {}

uint64_t Cursor::seekTo(uint64_t position) {

}

uint64_t Cursor::seekToFirst() {}
uint64_t Cursor::seekToLast() {}
void Cursor::getRow(const std::function<void (const uint8_t* data,
    size_t len, uint64_t time)>& func) const {}
bool Cursor::next() {}

uint64_t Cursor::appendRow(const std::vector<uint8_t>& data) {
  stream_ref_->appendRow(data);
}

std::unique_ptr<IStorageCursor> Cursor::clone() const {}

StreamRef::StreamRef(
    FileBackend* backend,
    uint64_t stream_id,
    const std::string& stream_key) :
    backend_(backend),
    stream_id_(stream_id),
    stream_key_(stream_key) {}

// FIXPAUL hold append lock
void StreamRef::appendRow(const std::vector<uint8_t>& data) {
  uint64_t time = WallClock::getUnixMillis();
  size_t row_size = data.size() + 16;

  if (pages_.size() == 0) {
    PageAlloc alloc;
    // FIXPAUL estimate size
    alloc.page = backend_->page_manager_.allocPage(data.size() * 100);
    alloc.used = 0;
    alloc.t0   = time;
    alloc.t1   = time;

    pages_.push_back(std::move(alloc));
    // FIXPAUL log: first page
  }

  if (pages_.back().used + row_size > pages_.back().page.size) {
    PageAlloc alloc;
    // FIXPAUL estimate size
    alloc.page = backend_->page_manager_.allocPage(data.size() * 100);
    alloc.used = 0;
    alloc.t0   = time;
    alloc.t1   = time;

    pages_.push_back(std::move(alloc));
    // FIXPAUL log: page finish
    // FIXPAUL log: new page
  }

  auto mmaped = backend_->mmap_manager_.getPage(pages_.back().page);

  RowHeader* row = mmaped.structAt<RowHeader>(pages_.back().used);
  row->time = time;
  row->size = data.size();
  memcpy(row->data, data.data(), row->size);

  pages_.back().used += row_size;
}

PageManager::PageManager(size_t end_pos, size_t block_size) :
  end_pos_(end_pos),
  block_size_(block_size) {}

PageManager::PageManager(const PageManager&& move) :
  end_pos_(move.end_pos_),
  block_size_(move.block_size_),
  freelist_(std::move(move.freelist_)) {}

// FIXPAUL hold lock!
PageManager::Page PageManager::allocPage(size_t min_size) {
  PageManager::Page page;

  /* align the request size to the next largest block boundary */
  uint64_t min_size_aligned =
      ((min_size + block_size_ - 1) / block_size_) * block_size_;

  if (!findFreePage(min_size_aligned, &page)) {
    page.offset = end_pos_;
    page.size   = min_size_aligned;
    //page.used   = 0;
    end_pos_   += page.size;
  }

  return page;
}

// FIXPAUL: proper freelist implementation
// FIXPAUL hold lock!
void PageManager::freePage(const PageManager::Page& page) {
  freelist_.emplace_back(std::make_pair(page.size, page.offset));
}

// FIXPAUL: proper freelist implementation
bool PageManager::findFreePage(size_t min_size, Page* destination) {
  for (auto iter = freelist_.begin(); iter != freelist_.end(); ++iter) {
    if (std::get<0>(*iter) >= min_size) {
      destination->offset = std::get<1>(*iter);
      destination->size   = std::get<0>(*iter);
      //destination->used   = 0;
      freelist_.erase(iter);
      return true;
    }
  }

  return false;
}

MmapPageManager::MmapPageManager(int fd, size_t len) :
    fd_(fd),
    file_size_(len),
    current_mapping_(nullptr) {}

MmapPageManager::MmapPageManager(MmapPageManager&& move) :
    fd_(move.fd_),
    file_size_(move.file_size_),
    current_mapping_(move.current_mapping_) {
  move.fd_ = -1;
  move.file_size_ = 0;
  move.current_mapping_ = nullptr;
}

MmapPageManager::~MmapPageManager() {
  if (current_mapping_ != nullptr) {
    current_mapping_->decrRefs();
  }
}

// FIXPAUL hold lock!
MmapPageManager::MmappedPageRef MmapPageManager::getPage(
    const PageManager::Page& page) {
  uint64_t last_byte = page.offset + page.size;
  // FIXPAUL: get mutex

  if (last_byte > file_size_) {
    ftruncate(fd_, last_byte); // FIXPAUL truncate in chunks + error checking
    file_size_ = last_byte;
  }

  return MmappedPageRef(page, getMmapedFile(last_byte));
}

MmapPageManager::MmappedFile* MmapPageManager::getMmapedFile(uint64_t last_byte) {
  if (current_mapping_ == nullptr || last_byte > current_mapping_->size) {
    /* align mmap size to the next larger block boundary */
    uint64_t mmap_size =
        ((last_byte + kMmapSizeMultiplier - 1) / kMmapSizeMultiplier) *
        kMmapSizeMultiplier;

    int fd = dup(fd_);
    if (fd < 0) {
      perror("dup() failed");
      abort(); // FIXPAUL
    }

    void* addr = mmap(
        nullptr,
        mmap_size,
        PROT_WRITE | PROT_READ,
        MAP_SHARED,
        fd,
        0);

    if (addr == MAP_FAILED) {
      perror("mmap() failed");
      abort(); // FIXPAUL
    }

    if (current_mapping_ != nullptr) {
      current_mapping_->decrRefs();
    }

    current_mapping_ = new MmappedFile(addr, mmap_size, fd);
  }

  return current_mapping_;
}

MmapPageManager::MmappedFile::MmappedFile(
  void* __data,
  const size_t __size,
  const int __fd) :
  data(__data),
  size(__size),
  fd(__fd),
  refs(1) {}

// FIXPAUL: locking!
void MmapPageManager::MmappedFile::incrRefs() {
  ++refs;
}

// FIXPAUL: locking!
void MmapPageManager::MmappedFile::decrRefs() {
  if (--refs == 0) {
    munmap(data, size);
    close(fd);
    delete this;
  }
}

MmapPageManager::MmappedPageRef::MmappedPageRef(
    const PageManager::Page& __page,
    MmappedFile* __file) :
    page(__page),
    file(__file) {
  file->incrRefs();
}

MmapPageManager::MmappedPageRef::MmappedPageRef(
    MmapPageManager::MmappedPageRef&& move) :
    page(move.page),
    file(move.file) {
  move.file = nullptr;
}

MmapPageManager::MmappedPageRef::~MmappedPageRef() {
  file->decrRefs();
}

void* MmapPageManager::MmappedPageRef::operator->() const {
  return file->data;
}

void* MmapPageManager::MmappedPageRef::operator*() const {
  return file->data;
}

}
}