//===----------------------------------------------------------------------===//
//
//                         Peloton
//
// compressed_tile.cpp
//
// Identification: src/storage/compressed_tile.cpp
//
// Copyright (c) 2015-16, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include <cstdio>
#include <sstream>

#include "catalog/schema.h"
#include "common/exception.h"
#include "common/macros.h"
#include "type/serializer.h"
#include "type/types.h"
#include "type/ephemeral_pool.h"
#include "concurrency/transaction_manager_factory.h"
#include "storage/storage_manager.h"
#include "storage/tile.h"
#include "storage/tile_group_header.h"
#include "storage/tuple.h"
#include "storage/tuple_iterator.h"
#include "storage/compressed_tile.h"
#include "container/cuckoo_map.h"

namespace peloton {
namespace storage {

bool CompareLessThanBool(type::Value left, type::Value right) {
  return left.CompareLessThan(right);
}

CompressedTile::CompressedTile(BackendType backend_type,
                               TileGroupHeader *tile_header,
                               const catalog::Schema &tuple_schema,
                               TileGroup *tile_group, int tuple_count)
    : Tile(backend_type, tile_header, tuple_schema, tile_group, tuple_count) {
  is_compressed = false;  // tile is currently uncompressed.
  compressed_columns_count = 0;
}

CompressedTile::~CompressedTile() {}

void CompressedTile::CompressTile(Tile *tile) {
  auto allocated_tuple_count = tile->GetAllocatedTupleCount();
  std::vector<type::Value> new_column_values(allocated_tuple_count);
  std::vector<std::vector<type::Value>> new_columns(column_count);
  std::vector<catalog::Column> columns;
  type::Value max_exponent_count;
  std::vector<type::Value> column_values;
  auto tile_schema = tile->GetSchema();

  for (oid_t i = 0; i < column_count; i++) {
    LOG_TRACE("Compressing Column %s",
              tile_schema->GetColumn(i).GetName().c_str());
    switch (tile_schema->GetType(i)) {
      case type::Type::SMALLINT:
      case type::Type::INTEGER:
      case type::Type::BIGINT:
        column_values = GetIntegerColumnValues(tile, i);
        new_column_values = CompressColumn(tile,i , column_values, type::Type::TINYINT);
        if (new_column_values.size() != 0) {
          compressed_columns_count += 1;
          type::Value old_value = tile->GetValue(0, i);
          type::Value new_value = new_column_values[0];

          type::Value base_value = GetBaseValue(old_value, new_value);
          type::Type::TypeId type_id = GetCompressedType(new_value);

          LOG_TRACE("Compressed %s to %s",
                    peloton::TypeIdToString(tile_schema->GetType(i)).c_str(),
                    peloton::TypeIdToString(type_id).c_str());

          SetCompressedMapValue(i, type_id, base_value);

        } else {
          LOG_TRACE("Unable to compress %s ",
                    peloton::TypeIdToString(tile_schema->GetType(i)).c_str());
        }
        new_columns[i] = new_column_values;
        break;
    case type::Type::DECIMAL:
        LOG_INFO("Peloton now compresses %s",
                  peloton::TypeIdToString(tile_schema->GetType(i)).c_str());
        max_exponent_count = GetMaxExponentLength(tile,i);
        column_values = ConvertDecimalColumn(tile, i, max_exponent_count);
        new_column_values = CompressColumn(tile, i , column_values, type::Type::TINYINT);
        if (new_column_values.size() !=0) {
          compressed_columns_count += 1;
          type::Value old_value = tile->GetValue(0, i);
          type::Value new_value = new_column_values[0];

          type::Value base_value = GetBaseValue(old_value, new_value.CastAs(tile_schema->GetType(i)).Divide(max_exponent_count));
          type::Type::TypeId type_id = GetCompressedType(new_value);
          LOG_INFO("Old Value : %s and New Value : %s\n", old_value.ToString().c_str(), new_value.ToString().c_str());
          LOG_INFO("Base Value : %s\n", base_value.ToString().c_str());
          LOG_INFO("Compressed %s to %s",
                    peloton::TypeIdToString(tile_schema->GetType(i)).c_str(),
                    peloton::TypeIdToString(type_id).c_str());

          SetCompressedMapValue(i, type_id, base_value);
          SetExponentMapValue(i, max_exponent_count);

          } else {
            LOG_INFO("Unable to compress %s ",
                    peloton::TypeIdToString(tile_schema->GetType(i)).c_str());
          }
          new_columns[i] = new_column_values;
          break;
    default:
        LOG_TRACE("Peloton currently does not compress %s",
                  peloton::TypeIdToString(tile_schema->GetType(i)).c_str());
        new_column_values.resize(0);
        new_columns[i] = new_column_values;
        break;
    }
  }

  if (compressed_columns_count != 0) {
    for (oid_t i = 0; i < column_count; i++) {
      // Get Column Info
      auto tile_column = tile_schema->GetColumn(i);
      // Individual Column info
      auto column_type = tile_column.GetType();
      auto column_type_size = tile_column.GetLength();
      auto column_name = tile_column.GetName();
      bool column_is_inlined = tile_column.IsInlined();

      if (new_columns[i].size() == 0) {
        catalog::Column column(column_type, column_type_size, column_name,
                               column_is_inlined);
        columns.push_back(column);
      } else {
        type::Type::TypeId new_column_type = GetCompressedType(i);
        catalog::Column column(new_column_type,
                               type::Type::GetTypeSize(new_column_type),
                               column_name, column_is_inlined);
        columns.push_back(column);
      }
    }

    auto &storage_manager = storage::StorageManager::GetInstance();

    LOG_TRACE("Size of the tile before compression: %zu", tile_size);

    // Reclaim data from old schema
    storage_manager.Release(backend_type, data);
    data = NULL;

    schema = catalog::Schema(columns);
    tuple_length = schema.GetLength();
    tile_size = num_tuple_slots * tuple_length;

    LOG_TRACE("Size of the tile after compression: %zu", tile_size);

    data = reinterpret_cast<char *>(
        storage_manager.Allocate(backend_type, tile_size));

    PL_ASSERT(data != NULL);
    // zero out the data
    PL_MEMSET(data, 0, tile_size);

    for (oid_t i = 0; i < column_count; i++) {
      bool is_inlined = schema.IsInlined(i);
      size_t new_column_offset = schema.GetOffset(i);

      if (GetCompressedType(i) == type::Type::INVALID) {
        size_t old_column_offset = tile_schema->GetOffset(i);
        auto old_column_type = tile_schema->GetColumn(i).GetType();

        for (oid_t j = 0; j < num_tuple_slots; j++) {
          type::Value old_value = tile->GetValueFast(
              j, old_column_offset, old_column_type, is_inlined);
          Tile::SetValueFast(old_value, j, new_column_offset, old_column_type,
                             is_inlined);
        }

      } else {
        auto new_column_type = schema.GetColumn(i).GetType();

        for (oid_t j = 0; j < num_tuple_slots; j++) {
          Tile::SetValueFast(new_columns[i][j], j, new_column_offset,
                             new_column_type, is_inlined);
        }
      }
    }

    for (oid_t i = 0; i < column_count; i++)
    {
      column_offset_map[schema.GetOffset(i)] = i;
    }

    is_compressed = true;
  }
}

std::vector<type::Value> CompressedTile::ConvertDecimalColumn(Tile *tile, oid_t column_id, type::Value exponent) {
  LOG_INFO("Called");
  oid_t num_tuples = tile->GetAllocatedTupleCount();
  auto tile_schema = tile->GetSchema();
  bool is_inlined = tile_schema->IsInlined(column_id);
  size_t column_offset = tile_schema->GetOffset(column_id);
  auto column_type = tile_schema->GetType(column_id);
  type::Value decimal_value;
  std::vector<type::Value> values(num_tuples);

  for (oid_t i = 0; i < num_tuples; i++) {
    decimal_value = tile->GetValueFast(i, column_offset, column_type, is_inlined);
    LOG_INFO("Old Decimal Value = %s", decimal_value.ToString().c_str());
    values[i] = decimal_value.Multiply(exponent);
    LOG_INFO("New Integer Value = %s", values[i].ToString().c_str());
  }
  return values;
}

std::vector<type::Value> CompressedTile::GetIntegerColumnValues(Tile *tile, oid_t column_id) {

  oid_t num_tuples = tile->GetAllocatedTupleCount();
  auto tile_schema = tile->GetSchema();
  bool is_inlined = tile_schema->IsInlined(column_id);
  size_t column_offset = tile_schema->GetOffset(column_id);
  auto column_type = tile_schema->GetType(column_id);

  std::vector<type::Value> column_values(num_tuples);

  for (oid_t i = 0; i < num_tuples; i++) {
    column_values[i] =
        tile->GetValueFast(i, column_offset, column_type, is_inlined);
  }
  return column_values;
}

type::Value CompressedTile::GetMaxExponentLength(Tile *tile, oid_t column_id) {
  LOG_INFO("Called");
  oid_t num_tuples = tile->GetAllocatedTupleCount();
  auto tile_schema = tile->GetSchema();
  bool is_inlined = tile_schema->IsInlined(column_id);
  size_t column_offset = tile_schema->GetOffset(column_id);
  auto column_type = tile_schema->GetType(column_id);
  
  type::Value decimal_value;
  type::Value increment = type::ValueFactory::GetTinyIntValue(10);
  
  type::Value current_max = type::ValueFactory::GetBigIntValue(1);

  LOG_INFO("Current Max Value = %s", current_max.ToString().c_str());

  for (oid_t i = 0; i < num_tuples; i++) {
    decimal_value = tile->GetValueFast(i, column_offset, column_type, is_inlined);
    LOG_INFO("Decimal Value = %s", decimal_value.ToString().c_str());
    type::Value new_value = decimal_value.Multiply(current_max);
    LOG_INFO("New Value = %s", new_value.ToString().c_str());
    while (new_value.CompareNotEquals(new_value.CastAs(type::Type::BIGINT))) {
      current_max = current_max.Multiply(increment);
      new_value = new_value.Multiply(current_max);
      LOG_INFO("New Maximum Value = %s", current_max.ToString().c_str());
    }
  }
  LOG_INFO("Final Maximum Value = %s", current_max.ToString().c_str());
  return current_max;
}


std::vector<type::Value> CompressedTile::CompressColumn(
    Tile *tile,
    oid_t column_id,
    std::vector<type::Value> column_values,
    type::Type::TypeId compression_type = type::Type::TINYINT) {

  oid_t num_tuples = tile->GetAllocatedTupleCount();
  auto tile_schema = tile->GetSchema();
  auto column_type = tile_schema->GetType(column_id);
  LOG_INFO("Old Value Type : %d", column_type);
  std::vector<type::Value> actual_values(column_values);

  std::sort(column_values.begin(), column_values.end(), CompareLessThanBool);

  type::Value median = column_values[num_tuples / 2];
  std::vector<type::Value> modified_values(num_tuples);

  while (true) {
    try {
      column_values[0].Subtract(median).CastAs(compression_type);
      column_values[num_tuples - 1].Subtract(median).CastAs(compression_type);
      for (oid_t k = 0; k < num_tuples; k++) {
        modified_values[k] =
            actual_values[k].Subtract(median).CastAs(compression_type);
      }

      LOG_INFO("Base value selected = %s", median.ToString().c_str());
      break;
    } catch (Exception &e) {
      if (column_type == (compression_type + 1)) {
        modified_values.resize(0);
        break;
      }
      compression_type = static_cast<type::Type::TypeId>(
          static_cast<int>(compression_type) + 1);
    }
  }
  LOG_INFO("New Value Type : %d", compression_type);
  return modified_values;
}

// compression for varchar
std::vector<type::Value> CompressedTile::CompressCharColumn(Tile *tile,
                                                            oid_t column_id) {
  oid_t num_tuples = tile->GetAllocatedTupleCount();
  auto tile_schema = tile->GetSchema();
  bool is_inlined = tile_schema->IsInlined(column_id);
  size_t column_offset = tile_schema->GetOffset(column_id);
  auto column_type = tile_schema->GetType(column_id);
  std::vector<type::Value> column_values(num_tuples);

  /* to use this template
   * src/container/cuckoo_map.cpp
   * needs to be modified and add this type
   */
  CuckooMap<std::string, int> dictionary;
  //TODO save the decoder somewhere
  std::vector<std::string> decoder;
  std::vector<type::Value> modified_values(num_tuples);

  int counter = 0;
  int new_value = 0;
  for (oid_t i = 0; i < num_tuples; i++) {
    std::string word = tile->GetValueFast(i, column_offset, column_type,
                                          is_inlined).ToString();

    // add a new word to dictionary
    if (dictionary.Contains(word) == false) {
     dictionary.Insert(word, counter);
      decoder.push_back(word);
      new_value = counter;
      counter++;
      LOG_DEBUG("new word: #%d : %s", counter, word.c_str());

    } else {
		//TODO fetch value from cuckoo
      new_value = 0;
    }
    //TODO modified_values[i] =
	(void) new_value;
  }

  return modified_values;
}

void CompressedTile::InsertTuple(const oid_t tuple_offset, Tuple *tuple) {
  if (IsCompressed()) {
    LOG_TRACE("Peloton does not support insert into compressed tiles.");
    PL_ASSERT(false);
  } else {
    Tile::InsertTuple(tuple_offset, tuple);
  }
}

type::Value CompressedTile::GetValue(const oid_t tuple_offset,
                                     const oid_t column_id) {
  type::Value deserializedValue = Tile::GetValue(tuple_offset, column_id);

  if (!IsCompressed()) {
    return deserializedValue;
  }

  if (GetCompressedType(column_id) == type::Type::INVALID) {
    return deserializedValue;
  }

  type::Type::TypeId compressed_type = GetCompressedType(column_id);

  PL_ASSERT(compressed_type != type::Type::INVALID);

  type::Value original = GetUncompressedValue(column_id, deserializedValue);

  return original;
}

type::Value CompressedTile::GetValueFast(const oid_t tuple_offset,
                                         const size_t column_offset,
                                         const type::Type::TypeId column_type,
                                         const bool is_inlined) {
  type::Value deserializedValue =
      Tile::GetValueFast(tuple_offset, column_offset, column_type, is_inlined);

  if (!IsCompressed()) {
    return deserializedValue;
  }

  oid_t column_id = GetColumnFromOffset(column_offset);

  PL_ASSERT(column_id < column_count);

  if (GetCompressedType(column_id) != type::Type::INVALID) {
    return GetUncompressedValue(column_id, deserializedValue);
  }

  return deserializedValue;
}

void CompressedTile::SetValue(const type::Value &value,
                              const oid_t tuple_offset, const oid_t column_id) {
  if ((IsCompressed()) &&
      (GetCompressedType(column_id) != type::Type::INVALID)) {
    LOG_TRACE("Peloton does not support SetValue on a Compressed Class");
    PL_ASSERT(false);
  }

  Tile::SetValue(value, tuple_offset, column_id);
}

void CompressedTile::SetValueFast(const type::Value &value,
                                  const oid_t tuple_offset,
                                  const size_t column_offset,
                                  const bool is_inlined,
                                  const size_t column_length) {
  oid_t column_id = GetColumnFromOffset(column_offset);

  PL_ASSERT(column_id < column_count);

  if (IsCompressed()) {
    if (GetCompressedType(column_id) != type::Type::INVALID) {
      LOG_TRACE("Peloton does not support");
      PL_ASSERT(false);
    }
  }

  Tile::SetValueFast(value, tuple_offset, column_offset, is_inlined,
                     column_length);
}
}
}