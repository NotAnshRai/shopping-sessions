#pragma once
#include "input.hpp"
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/writer.h>

inline void check(arrow::Status status) {
    if (!status.ok()) throw std::runtime_error(status.ToString());
}

template<class T> T value(arrow::Result<T> result) {
    if (!result.ok()) throw std::runtime_error(result.status().ToString());
    return std::move(result).ValueUnsafe();
}

class BatchFile {
    std::shared_ptr<arrow::Schema> schema_;
    std::shared_ptr<arrow::io::FileOutputStream> sink_;
    std::unique_ptr<parquet::arrow::FileWriter> writer_;
    std::vector<arrow::ArrayBuilder*> columns_;
public:
    BatchFile(const fs::path& path, std::shared_ptr<arrow::Schema> schema,
              std::vector<arrow::ArrayBuilder*> columns)
        : schema_(std::move(schema)), columns_(std::move(columns)) {
        sink_ = value(arrow::io::FileOutputStream::Open(path.string()));
        auto props = parquet::WriterProperties::Builder().disable_dictionary()->disable_statistics()->build();
        writer_ = value(parquet::arrow::FileWriter::Open(*schema_, arrow::default_memory_pool(), sink_, props));
    }
    void flush() {
        if (!columns_.front()->length()) return;
        std::vector<std::shared_ptr<arrow::Array>> arrays;
        for (auto* col : columns_) arrays.emplace_back(value(col->Finish()));
        check(writer_->WriteTable(*arrow::Table::Make(schema_, arrays), 65536));
    }
    void maybe_flush() { if (columns_.front()->length() >= 65536) flush(); }
    void close() { flush(); check(writer_->Close()); check(sink_->Close()); }
};

class UserOutput {
    arrow::StringBuilder name_, category_;
    arrow::Date32Builder day_;
    arrow::Int64Builder sessions_, events_, revenue_;
    BatchFile file_;
public:
    explicit UserOutput(const fs::path& path) : file_(path, arrow::schema({
        arrow::field("user_id", arrow::utf8()), arrow::field("day", arrow::date32()),
        arrow::field("sessions", arrow::int64()), arrow::field("events", arrow::int64()),
        arrow::field("revenue_cents", arrow::int64()), arrow::field("top_category", arrow::utf8())}),
        {&name_, &day_, &sessions_, &events_, &revenue_, &category_}) {}
    void add(Text person, int day, I64 sessions, I64 events, I64 revenue, const Text* category) {
        check(name_.Append(person)); check(day_.Append(day)); check(sessions_.Append(sessions));
        check(events_.Append(events)); check(revenue_.Append(revenue));
        check(category ? category_.Append(*category) : category_.AppendNull());
        file_.maybe_flush();
    }
    void close() { file_.close(); }
};

struct Metrics {
    I64 sessions = 0, purchasing = 0, revenue = 0, assists = 0;
    void merge(const Metrics& m) {
        sessions += m.sessions; purchasing += m.purchasing; revenue += m.revenue; assists += m.assists;
    }
};

class CampaignOutput {
    arrow::StringBuilder id_, channel_;
    arrow::Date32Builder day_;
    arrow::Int64Builder sessions_, purchasing_, revenue_, assists_;
    BatchFile file_;
public:
    explicit CampaignOutput(const fs::path& path) : file_(path, arrow::schema({
        arrow::field("campaign_id", arrow::utf8()), arrow::field("channel", arrow::utf8()),
        arrow::field("day", arrow::date32()), arrow::field("sessions", arrow::int64()),
        arrow::field("purchasing_sessions", arrow::int64()), arrow::field("revenue_cents", arrow::int64()),
        arrow::field("assisted_sessions", arrow::int64())}),
        {&id_, &channel_, &day_, &sessions_, &purchasing_, &revenue_, &assists_}) {}
    void add(Text id, Text channel, int day, const Metrics& m) {
        check(id_.Append(id)); check(channel_.Append(channel)); check(day_.Append(day));
        check(sessions_.Append(m.sessions)); check(purchasing_.Append(m.purchasing));
        check(revenue_.Append(m.revenue)); check(assists_.Append(m.assists)); file_.maybe_flush();
    }
    void close() { file_.close(); }
};
