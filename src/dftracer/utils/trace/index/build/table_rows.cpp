// SPDX-License-Identifier: MIT
#include <dftracer/utils/trace/index/build/table_rows.h>
#include <dftracer/utils/trace/table/table_header.h>

namespace dftracer::utils::trace::index::build {

dstd::SharedPtr<const TableShape> TableShape::from_header_line(
    dstd::StringView line) {
    if (line.find(table::detail::FORMAT) == dstd::StringView::npos)
        return nullptr;
    dstd::String text(line);
    const std::size_t capacity = text.size() + simdjson::SIMDJSON_PADDING;
    text.resize(capacity, '\0');
    json::Document doc;
    auto root = doc.parse_in_place(dstd::StringView(text.data(), line.size()),
                                   capacity);
    if (!root) return nullptr;
    auto args = root->find("args");
    if (!args) return nullptr;
    auto header = table::detail::parse_header(*args);
    if (!header) return nullptr;
    auto shape = dstd::make_shared<TableShape>();
    shape->nrows_ = header->nrows;
    for (std::size_t i = 0; i < header->fields.size(); ++i)
        shape->field_index_.emplace(header->fields[i].name, i);
    // A repeated field name cannot be told apart in a row.
    if (shape->field_index_.size() != header->fields.size()) return nullptr;
    return shape;
}

TableRowCheck::TableRowCheck(const TableShape& shape)
    : shape_(&shape), seen_(shape.field_index_.size(), 0) {}

void TableRowCheck::begin_row() {
    matched_ = 0;
    if (++stamp_ == 0) {
        for (auto& s : seen_) s = 0;
        stamp_ = 1;
    }
}

void TableRowCheck::see(dstd::StringView name) {
    auto it = shape_->field_index_.find(name);
    if (it == shape_->field_index_.end()) return;
    std::uint32_t& slot = seen_[it->second];
    if (slot == stamp_) return;
    slot = stamp_;
    ++matched_;
}

void TableRowCheck::end_row() {
    if (matched_ == shape_->field_index_.size()) ++complete_;
}

void TableRowCheck::row(simdjson::dom::element root) {
    std::int64_t ph = 0;
    if (root["ph"].get(ph) || ph != 3) return;
    simdjson::dom::object args;
    if (root["args"].get(args)) return;
    begin_row();
    for (auto field : args) see(field.key);
    end_row();
}

void TableRowCheck::row(json::Value& root) {
    auto ph = root.find("ph");
    if (!ph) return;
    auto phv = ph->as_int();
    if (!phv || *phv != 3) return;
    auto args = root.find("args");
    if (!args || args->type() != json::Value::Type::Object) return;
    begin_row();
    args->for_each_member(
        [&](dstd::StringView name, json::Value) { see(name); });
    end_row();
}

}  // namespace dftracer::utils::trace::index::build
