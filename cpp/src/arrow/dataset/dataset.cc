// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "arrow/dataset/dataset.h"

#include <memory>
#include <utility>

#include "arrow/dataset/dataset_internal.h"
#include "arrow/dataset/filter.h"
#include "arrow/dataset/scanner.h"
#include "arrow/util/bit_util.h"
#include "arrow/util/iterator.h"
#include "arrow/util/make_unique.h"

namespace arrow {
namespace dataset {

Fragment::Fragment(std::shared_ptr<ScanOptions> scan_options)
    : scan_options_(std::move(scan_options)), partition_expression_(scalar(true)) {}

const std::shared_ptr<Schema>& Fragment::schema() const {
  return scan_options_->schema();
}

InMemoryFragment::InMemoryFragment(
    std::vector<std::shared_ptr<RecordBatch>> record_batches,
    std::shared_ptr<ScanOptions> scan_options)
    : Fragment(std::move(scan_options)), record_batches_(std::move(record_batches)) {}

Result<ScanTaskIterator> InMemoryFragment::Scan(std::shared_ptr<ScanContext> context) {
  // Make an explicit copy of record_batches_ to ensure Scan can be called
  // multiple times.
  auto batches_it = MakeVectorIterator(record_batches_);

  // RecordBatch -> ScanTask
  auto scan_options = scan_options_;
  auto fn = [=](std::shared_ptr<RecordBatch> batch) -> std::shared_ptr<ScanTask> {
    std::vector<std::shared_ptr<RecordBatch>> batches{batch};
    return ::arrow::internal::make_unique<InMemoryScanTask>(
        std::move(batches), std::move(scan_options), std::move(context));
  };

  return MakeMapIterator(fn, std::move(batches_it));
}

Result<std::shared_ptr<Dataset>> Dataset::Make(SourceVector sources,
                                               std::shared_ptr<Schema> schema) {
  return std::shared_ptr<Dataset>(new Dataset(std::move(sources), std::move(schema)));
}

Result<std::shared_ptr<ScannerBuilder>> Dataset::NewScan(
    std::shared_ptr<ScanContext> context) {
  return std::make_shared<ScannerBuilder>(this->shared_from_this(), context);
}

Result<std::shared_ptr<ScannerBuilder>> Dataset::NewScan() {
  return NewScan(std::make_shared<ScanContext>());
}

bool Source::AssumePartitionExpression(
    const std::shared_ptr<ScanOptions>& scan_options,
    std::shared_ptr<ScanOptions>* simplified_scan_options) const {
  if (partition_expression_ == nullptr) {
    if (simplified_scan_options != nullptr) {
      *simplified_scan_options = scan_options;
    }
    return true;
  }

  auto expr = scan_options->filter->Assume(*partition_expression_);
  if (expr->IsNull() || expr->Equals(false)) {
    // selector is not satisfiable; yield no fragments
    return false;
  }

  if (simplified_scan_options != nullptr) {
    auto copy = std::make_shared<ScanOptions>(*scan_options);
    copy->filter = std::move(expr);
    *simplified_scan_options = std::move(copy);
  }
  return true;
}

FragmentIterator Source::GetFragments(std::shared_ptr<ScanOptions> scan_options) {
  std::shared_ptr<ScanOptions> simplified_scan_options;
  if (!AssumePartitionExpression(scan_options, &simplified_scan_options)) {
    return MakeEmptyIterator<std::shared_ptr<Fragment>>();
  }
  return GetFragmentsImpl(std::move(simplified_scan_options));
}

struct VectorRecordBatchGenerator : InMemorySource::RecordBatchGenerator {
  explicit VectorRecordBatchGenerator(std::vector<std::shared_ptr<RecordBatch>> batches)
      : batches_(std::move(batches)) {}

  RecordBatchIterator Get() const final { return MakeVectorIterator(batches_); }

  std::vector<std::shared_ptr<RecordBatch>> batches_;
};

InMemorySource::InMemorySource(std::shared_ptr<Schema> schema,
                               std::vector<std::shared_ptr<RecordBatch>> batches)
    : Source(std::move(schema)),
      get_batches_(new VectorRecordBatchGenerator(std::move(batches))) {}

struct TableRecordBatchGenerator : InMemorySource::RecordBatchGenerator {
  explicit TableRecordBatchGenerator(std::shared_ptr<Table> table)
      : table_(std::move(table)) {}

  RecordBatchIterator Get() const final {
    auto reader = std::make_shared<TableBatchReader>(*table_);
    auto table = table_;
    return MakeFunctionIterator([reader, table] { return reader->Next(); });
  }

  std::shared_ptr<Table> table_;
};

InMemorySource::InMemorySource(std::shared_ptr<Table> table)
    : Source(table->schema()),
      get_batches_(new TableRecordBatchGenerator(std::move(table))) {}

FragmentIterator InMemorySource::GetFragmentsImpl(
    std::shared_ptr<ScanOptions> scan_options) {
  auto schema = this->schema();

  auto create_fragment =
      [scan_options,
       schema](std::shared_ptr<RecordBatch> batch) -> Result<std::shared_ptr<Fragment>> {
    if (!batch->schema()->Equals(schema)) {
      return Status::TypeError("yielded batch had schema ", *batch->schema(),
                               " which did not match InMemorySource's: ", *schema);
    }

    std::vector<std::shared_ptr<RecordBatch>> batches;

    auto batch_size = scan_options->batch_size;
    auto n_batches = BitUtil::CeilDiv(batch->num_rows(), batch_size);

    for (int i = 0; i < n_batches; i++) {
      batches.push_back(batch->Slice(batch_size * i, batch_size));
    }

    return std::make_shared<InMemoryFragment>(std::move(batches), scan_options);
  };

  return MakeMaybeMapIterator(std::move(create_fragment), get_batches_->Get());
}

FragmentIterator TreeSource::GetFragmentsImpl(std::shared_ptr<ScanOptions> options) {
  return GetFragmentsFromSources(children_, options);
}

}  // namespace dataset
}  // namespace arrow
