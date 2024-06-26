// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// All rights reserved.
//
// SPDX-License-Identifier: BSD-3-Clause
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor the names of their
// contributors may be used to endorse or promote products derived from this
// software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#ifndef BESS_MODULES_WILDCARDMATCH_H_
#define BESS_MODULES_WILDCARDMATCH_H_

#include "../module.h"

#include <rte_config.h>
#include <rte_hash_crc.h>

#include "../pb/module_msg.pb.h"
#include "../utils/cuckoo_map.h"

using bess::utils::CuckooMap;
using bess::utils::HashResult;

#define MAX_TUPLES 16
#define MAX_FIELDS 8
#define MAX_FIELD_SIZE 8
#define BULK_SIZE 32
static_assert(MAX_FIELD_SIZE <= sizeof(uint64_t),
              "field cannot be larger than 8 bytes");

#define HASH_KEY_SIZE (MAX_FIELDS * MAX_FIELD_SIZE)

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error this code assumes little endian architecture (x86)
#endif

struct WmField {
  int attr_id; /* -1 for offset-based fields */

  /* Relative offset in the packet data for offset-based fields.
   *  (starts from data_off, not the beginning of the headroom */
  int offset;

  int pos; /* relative position in the key */

  int size; /* in bytes. 1 <= size <= MAX_FIELD_SIZE */
};

struct wm_hkey_t {
  uint64_t u64_arr[MAX_FIELDS];
};

struct WmData {
  int priority;
  gate_idx_t ogate;
  wm_hkey_t keyv;
};

class wm_eq {
 public:
  explicit wm_eq(size_t len) : len_(len) {}

  bool operator()(const wm_hkey_t &lhs, const wm_hkey_t &rhs) const {
    promise(len_ >= sizeof(uint64_t));
    promise(len_ <= sizeof(wm_hkey_t));

    for (size_t i = 0; i < len_ / 8; i++) {
// Disable uninitialized variable checking in GCC due to false positives
#if !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
      if (lhs.u64_arr[i] != rhs.u64_arr[i]) {
#if !defined(__clang__)
#pragma GCC diagnostic pop
#endif
        return false;
      }
    }
    return true;
  }

 private:
  size_t len_;
};

class wm_hash {
 public:
  wm_hash() : len_(sizeof(wm_hkey_t)) {}
  explicit wm_hash(size_t len) : len_(len) {}

  HashResult operator()(const wm_hkey_t &key) const {
    HashResult init_val = 0;

    promise(len_ >= sizeof(uint64_t));
    promise(len_ <= sizeof(wm_hkey_t));

#if __x86_64
    for (size_t i = 0; i < len_ / 8; i++) {
      init_val = crc32c_sse42_u64(key.u64_arr[i], init_val);
    }
    return init_val;
#else
    return rte_hash_crc(&key, len_, init_val);
#endif
  }

 private:
  size_t len_;
};
struct rte_hash_parameters dpdk_params1 {
  .name = "test2", .entries = 1 << 15, .reserved = 0,
  .key_len = sizeof(wm_hkey_t), .hash_func = rte_hash_crc,
  .hash_func_init_val = 0, .socket_id = (int)rte_socket_id(),
  .extra_flag = RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY
};

class WildcardMatch final : public Module {
 public:
  static const gate_idx_t kNumOGates = MAX_GATES;

  static const Commands cmds;

  WildcardMatch()
      : Module(),
        default_gate_(),
        total_key_size_(),
        total_value_size_(),
        fields_(),
        values_(),
        tuples_() {
    max_allowed_workers_ = Worker::kMaxWorkers;
    { tuples_.resize(MAX_TUPLES); }
  }

  CommandResponse Init(const bess::pb::WildcardMatchArg &arg);

  void DeInit() override;

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

  std::string GetDesc() const override;

  CommandResponse GetInitialArg(const bess::pb::EmptyArg &arg);
  CommandResponse GetRuntimeConfig(const bess::pb::EmptyArg &arg);
  CommandResponse SetRuntimeConfig(const bess::pb::WildcardMatchConfig &arg);
  CommandResponse CommandAdd(const bess::pb::WildcardMatchCommandAddArg &arg);
  CommandResponse CommandDelete(
      const bess::pb::WildcardMatchCommandDeleteArg &arg);
  CommandResponse CommandClear(const bess::pb::EmptyArg &arg);
  CommandResponse CommandSetDefaultGate(
      const bess::pb::WildcardMatchCommandSetDefaultGateArg &arg);

 private:
  struct WmTuple {
    bool occupied;
    CuckooMap<wm_hkey_t, struct WmData, wm_hash, wm_eq> *ht;
    wm_hkey_t mask;
    struct rte_hash_parameters params;
    std::string hash_name;
    WmTuple() : occupied(0), ht(0) {
      params = dpdk_params1;
      std::ostringstream address;
      address << this;
      hash_name = "Wild" + address.str();
      params.name = hash_name.c_str();
    }
  };

  gate_idx_t LookupEntry(const wm_hkey_t &key, gate_idx_t def_gate,
                         bess::Packet *pkt);

  bool LookupBulkEntry(wm_hkey_t *key, gate_idx_t def_gate, int i,
                       gate_idx_t *Outgate, int cnt, bess::PacketBatch *batch);

  CommandResponse AddFieldOne(const bess::pb::Field &field, struct WmField *f,
                              uint8_t type);

  template <typename T>
  CommandResponse ExtractKeyMask(const T &arg, wm_hkey_t *key, wm_hkey_t *mask);
  template <typename T>
  CommandResponse ExtractValue(const T &arg, wm_hkey_t *keyv);

  int FindTuple(wm_hkey_t *mask);
  int AddTuple(wm_hkey_t *mask);
  bool DelEntry(int idx, wm_hkey_t *key);
  void Clear();
  gate_idx_t default_gate_;

  size_t total_key_size_;   /* a multiple of sizeof(uint64_t) */
  size_t total_value_size_; /* a multiple of sizeof(uint64_t) */
  size_t entries_;          /* a power of 2 */

  // TODO(melvinw): this can be refactored to use ExactMatchTable
  std::vector<struct WmField> fields_;
  std::vector<struct WmField> values_;
  std::vector<struct WmTuple> tuples_;  //[MAX_TUPLES];
  std::vector<struct WmData> data_;
};

#endif  // BESS_MODULES_WILDCARDMATCH_H_
