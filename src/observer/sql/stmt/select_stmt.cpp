/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Wangyunlai on 2022/6/6.
//

#include "sql/stmt/select_stmt.h"
#include "common/lang/string.h"
#include "common/log/log.h"
#include "sql/stmt/filter_stmt.h"
#include "storage/db/db.h"
#include "storage/table/table.h"

SelectStmt::~SelectStmt() {
  if (nullptr != filter_stmt_) {
    delete filter_stmt_;
    filter_stmt_ = nullptr;
  }
}

static void wildcard_fields(Table *table, std::vector<Field> &field_metas, std::vector<AliasCell> &alias_vec) {
  const TableMeta &table_meta = table->table_meta();
  const int field_num = table_meta.field_num();
  for (int i = table_meta.sys_field_num(); i < field_num; i++) {
    field_metas.emplace_back(table, table_meta.field(i));
    alias_vec.emplace_back();
  }
}

static void agg_builder_inner(std::vector<Field> &query_fields, int &agg_pos,
                              std::vector<std::pair<const FieldMeta *, int>> &aggregate_keys,
                              std::vector<agg> &aggregate_types, const RelAttrSqlNode &relation_attr, bool &agg_flag,
                              bool &non_agg_flag) {
  if (relation_attr.aggregate_func == agg::NONE) {
    // Do nothing if this is not a aggregation
    non_agg_flag = true;
    return;
  }

  agg_flag = true;

  assert(query_fields.size() > 0 && "The size of `query_fields` must greater than zero");
  assert(relation_attr.aggregate_func != agg::NONE);

  aggregate_keys.push_back({query_fields[agg_pos].meta(), query_fields.size() - agg_pos});
  agg_pos = query_fields.size();
  aggregate_types.push_back(relation_attr.aggregate_func);
}

RC bind_order_by(Db *db, const std::vector<Table *> &tables, const std::vector<OrderBySqlNode> &order_bys,
                 std::vector<OrderByStmt> &order_by_stmts) {
  for (const auto &order_by : order_bys) {
    auto &attr = order_by.order_by_attributes[0];
    bool is_asc = order_by.order_by_asc[0];
    const char *table_name = attr.relation_name.c_str();
    const char *field_name = attr.attribute_name.c_str();

    if (common::is_blank(table_name)) {
      // Table name is empty means that only single table is involved
      if (common::is_blank(field_name)) {
        // Field name is empty
        LOG_WARN("invalid order by. both table and field are blank");
        return RC::INVALID_ARGUMENT;
      }
      // find field in every table
      assert(tables.size() == 1 && "The size of `tables` must be one");
      const FieldMeta *field_meta = nullptr;
      for (Table *table : tables) {
        field_meta = table->table_meta().field(field_name);
        if (nullptr != field_meta) {
          order_by_stmts.push_back({Field(table, field_meta), is_asc});
          break;
        }
      }
      if (nullptr == field_meta) {
        LOG_WARN("no such field. field=%s.%s", tables[0]->name(), field_name);
        return RC::SCHEMA_FIELD_MISSING;
      }
    } else {
      // Table name is not empty eg : select t1.c1 from t1 order by t1.c1
      Table *table = db->find_table(table_name);
      if (nullptr == table) {
        LOG_WARN("no such table. db=%s, table_name=%s", db->name(), table_name);
        return RC::SCHEMA_TABLE_NOT_EXIST;
      }

      if (common::is_blank(field_name)) {
        LOG_WARN("invalid order by. field is blank");
        return RC::INVALID_ARGUMENT;
      }

      const FieldMeta *field_meta = table->table_meta().field(field_name);
      if (nullptr == field_meta) {
        LOG_WARN("no such field. field=%s.%s.%s", db->name(), table->name(), field_name);
        return RC::SCHEMA_FIELD_MISSING;
      }

      order_by_stmts.push_back({Field(table, field_meta), is_asc});
    }
  }

  return RC::SUCCESS;
}

RC SelectStmt::resolve_tables(Db *db, const SelectSqlNode &select_sql, std::vector<Table *> &tables,
                              std::unordered_map<std::string, Table *> &table_map,
                              std::unordered_map<std::string, std::string> &table_alias_map) {
  for (size_t i = 0; i < select_sql.relations.size(); i++) {
    const char *table_name = select_sql.relations[i].relation_name.c_str();
    const auto &alias = select_sql.relations[i].alias_name;
    // Both table name and alias name will map to the same table
    if (nullptr == table_name) {
      LOG_WARN("invalid argument. relation name is null. index=%d", i);
      return RC::INVALID_ARGUMENT;
    }

    Table *table = db->find_table(table_name);
    if (nullptr == table) {
      LOG_WARN("no such table. db=%s, table_name=%s", db->name(), table_name);
      return RC::SCHEMA_TABLE_NOT_EXIST;
    }

    tables.push_back(table);
    table_map.insert(std::pair<std::string, Table *>(table_name, table));
    if (!alias.empty()) {
      table_map.insert(std::pair<std::string, Table *>(alias, table));
      table_alias_map.insert(std::pair<std::string, std::string>(table_name, alias));
    }
  }
  return RC::SUCCESS;
}

/// TODO: We definitely need to refactor this part, the current implementation is so embarrassed 😅
RC SelectStmt::create(Db *db, const SelectSqlNode &select_sql, Stmt *&stmt) {
  assert(stmt == nullptr && "`stmt` must be nullptr at the beginning");
  if (nullptr == db) {
    LOG_WARN("invalid argument. db is null");
    return RC::INVALID_ARGUMENT;
  }

  // collect tables in `from` statement
  std::vector<Table *> tables;
  std::unordered_map<std::string, Table *> table_map;
  // Map table -> table's alias name
  std::unordered_map<std::string, std::string> table_alias_map;
  RC rc = resolve_tables(db, select_sql, tables, table_map, table_alias_map);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to resolve tables");
    return rc;
  }

  // collect query fields in `select` statement
  std::vector<Field> query_fields;
  std::vector<AliasCell> alias_vec;

  // For aggregation
  std::vector<std::pair<const FieldMeta *, int>> aggregate_keys;
  std::vector<agg> aggregate_types;
  int agg_pos = 0;
  bool agg_flag{false};
  bool non_agg_flag{false};

  // Resolve the query fields
  for (int i = static_cast<int>(select_sql.attributes.size()) - 1; i >= 0; i--) {
    const RelAttrSqlNode &relation_attr = select_sql.attributes[i];
    if (!relation_attr.agg_valid_flag) {
      // Invalid syntax
      return RC::INVALID_ARGUMENT;
    }

    if (common::is_blank(relation_attr.relation_name.c_str()) &&
        0 == strcmp(relation_attr.attribute_name.c_str(), "*")) {
      // If the current field is wildcard. (i.e., COUNT(*))
      for (Table *table : tables) {
        // We basically need all the metadata from all the underlying tables
        wildcard_fields(table, query_fields, alias_vec);
      }

      // Possibly aggregation on `*`
      agg_builder_inner(query_fields, agg_pos, aggregate_keys, aggregate_types, relation_attr, agg_flag, non_agg_flag);

    } else if (!common::is_blank(relation_attr.relation_name.c_str())) {
      // If the table name is not null. (i.e., `select t1.c1 from t1;`)
      const char *table_name = relation_attr.relation_name.c_str();
      const char *field_name = relation_attr.attribute_name.c_str();

      if (0 == strcmp(table_name, "*")) {
        if (0 != strcmp(field_name, "*")) {
          // Only `*.*` is permitted, but to be honest I do not know why to use this though. 🤣
          LOG_WARN("invalid field name while table is *. attr=%s", field_name);
          return RC::SCHEMA_FIELD_MISSING;
        }
        for (Table *table : tables) {
          wildcard_fields(table, query_fields, alias_vec);
        }

        // Essentially the same as `*` cases for aggregation
        agg_builder_inner(
            query_fields, agg_pos, aggregate_keys, aggregate_types, relation_attr, agg_flag, non_agg_flag);

      } else {
        // select t1.c1 from t1;
        auto iter = table_map.find(table_name);
        // Table alias is also in this map
        if (iter == table_map.end()) {
          LOG_WARN("no such table in from list: %s", table_name);
          return RC::SCHEMA_FIELD_MISSING;
        }

        Table *table = iter->second;
        if (0 == strcmp(field_name, "*")) {
          // i.e., `select t1.* from t1;`. Though this is essentially the same with `*`.
          wildcard_fields(table, query_fields, alias_vec);
          agg_builder_inner(
              query_fields, agg_pos, aggregate_keys, aggregate_types, relation_attr, agg_flag, non_agg_flag);
        } else {
          // It's real name ,not alias name
          const FieldMeta *field_meta = table->table_meta().field(field_name);
          if (nullptr == field_meta) {
            LOG_WARN("no such field. field=%s.%s.%s", db->name(), table->name(), field_name);
            return RC::SCHEMA_FIELD_MISSING;
          }
          query_fields.emplace_back(table, field_meta);
          if (!relation_attr.alias_name.empty()) {
            alias_vec.push_back({true, relation_attr.alias_name, table_name});
          } else {
            alias_vec.push_back(AliasCell());
          }
          agg_builder_inner(
              query_fields, agg_pos, aggregate_keys, aggregate_types, relation_attr, agg_flag, non_agg_flag);
        }
      }
    } else {
      // Only the column name is available. (i.e., `select c1 from t1;`)
      if (tables.size() != 1) {
        // Basically this is saying, i.e., `select c1 from t1 natural join t1;` is invalid.
        // Though this is a perfectly valid SQL syntax.
        LOG_WARN("invalid. I do not know the attr's table. attr=%s", relation_attr.attribute_name.c_str());
        return RC::SCHEMA_FIELD_MISSING;
      }

      assert(tables.size() == 1);

      Table *table = tables[0];
      const FieldMeta *field_meta = table->table_meta().field(relation_attr.attribute_name.c_str());
      if (nullptr == field_meta) {
        LOG_WARN("no such field. field=%s.%s.%s", db->name(), table->name(), relation_attr.attribute_name.c_str());
        return RC::SCHEMA_FIELD_MISSING;
      }

      query_fields.emplace_back(table, field_meta);
      if (!relation_attr.alias_name.empty()) {
        // Check if the table has an alias name
        auto iter = table_alias_map.find(table->name());
        if (iter != table_alias_map.end()) {
          // If the table has an alias name, use the alias name instead
          alias_vec.push_back({true, relation_attr.alias_name, iter->second});
        } else {
          // Does not have an alias name
          alias_vec.push_back({true, relation_attr.alias_name, table->name()});
        }
      } else {
        alias_vec.push_back(AliasCell());
      }
      agg_builder_inner(query_fields, agg_pos, aggregate_keys, aggregate_types, relation_attr, agg_flag, non_agg_flag);
    }
  }

  if (agg_flag && non_agg_flag) {
    return RC::INVALID_ARGUMENT;
  }

  if (agg_flag) {
    // Need to reverse the query_fields
    query_fields = {query_fields.rbegin(), query_fields.rend()};
  }

  assert(query_fields.size() == alias_vec.size());

  LOG_INFO("got %d tables in from stmt and %d fields in query stmt", tables.size(), query_fields.size());

  Table *default_table = nullptr;
  if (tables.size() == 1) {
    default_table = tables[0];
  }

  auto conditions = select_sql.conditions.data();
  auto condition_num = select_sql.conditions.size();

  // Check every equal-to condition ,then range the tables
  // To check if we can do any join operation
  std::vector<JoinStmt> join_stmts;
  for (size_t i = 0; i < condition_num; i++) {
    if (conditions[i].comp == CompOp::EQUAL_TO) {
      if (conditions[i].left_is_attr && conditions[i].right_is_attr) {
        // Range the tables
        std::string left_table_name = conditions[i].left_attr.relation_name;
        std::string right_table_name = conditions[i].right_attr.relation_name;
        if (left_table_name != right_table_name) {
          // Join operation
          FilterUnit *filter_unit = nullptr;
          RC rc = FilterStmt::create_filter_unit(
              db, default_table, &table_map, select_sql.attributes, conditions[i], filter_unit);
          auto filter_obj_left = filter_unit->left().field;
          auto filter_obj_right = filter_unit->right().field;
          if (rc != RC::SUCCESS) {
            LOG_WARN("failed to create filter unit. condition index=%d", i);
            return rc;
          }

          JoinStmt join_stmt = {filter_obj_left, filter_obj_right};
          join_stmts.push_back(join_stmt);
        }
      }
    }
  }

  // Bind order by
  std::vector<OrderByStmt> order_by_stmts;
  rc = bind_order_by(db, tables, select_sql.order_bys, order_by_stmts);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to bind order by");
    return rc;
  }

  // create filter statement in `where` statement
  FilterStmt *filter_stmt = nullptr;
  rc = FilterStmt::create(db,
                          default_table,
                          &table_map,
                          select_sql.attributes,
                          select_sql.conditions.data(),
                          static_cast<int>(select_sql.conditions.size()),
                          filter_stmt);
  if (rc != RC::SUCCESS) {
    LOG_WARN("cannot construct filter stmt");
    return rc;
  }

  // create aggregation statement
  AggStmt *agg_stmt{nullptr};
  if (agg_flag) {
    agg_stmt = new AggStmt{aggregate_keys, aggregate_types};
    assert(agg_stmt != nullptr && "`agg_stmt` must not be nullptr");
  }

  // everything alright
  SelectStmt *select_stmt = new SelectStmt();
  // TODO(maybe) change fields beacuse of length function or data_format
  // TODO add expression copy

  select_stmt->tables_.swap(tables);
  select_stmt->make_funcion_express(select_sql.attributes);
  select_stmt->query_fields_.swap(query_fields);
  select_stmt->alias_vec_.swap(alias_vec);
  select_stmt->filter_stmt_ = filter_stmt;
  select_stmt->agg_stmt_ = agg_stmt;
  select_stmt->join_stmts_ = join_stmts;
  select_stmt->order_by_ = order_by_stmts;
  stmt = select_stmt;
  return RC::SUCCESS;
}

std::vector<std::unique_ptr<Expression>>&& SelectStmt::make_funcion_express(const std::vector<RelAttrSqlNode>& attributes) {
  std::vector<std::unique_ptr<Expression>> change_functions;
  for(int j = 0; j < attributes.size(); j++) {
    int i = attributes.size() - j - 1;
    if(attributes[i].func != field_function::NOFUNC) {
      if(attributes[i].func == field_function::F_LENGTH) {
        if(attributes[i].length == -1) {
          change_functions.push_back(std::make_unique<LengthExpr>(i, attributes[i].char_name));
        } else {
          Value value(attributes[i].length);
          change_functions.push_back(std::make_unique<LengthExpr>(value, i, attributes[i].char_name));
        }
      }
      if(attributes[i].func == field_function::F_ROUND) {
        Value value(attributes[i].round_num);
        change_functions.push_back(std::make_unique<RoundExpr>(value, i, attributes[i].char_name));
      }
      if(attributes[i].func == field_function::F_DATA_FORMAT) {
        printf("make function %s \n",attributes[i].date_format.c_str());
        Value value(attributes[i].date_format.c_str());
        change_functions.push_back(std::make_unique<DataFormatExpr>(value, i, attributes[i].char_name));
      }
    }
  }
  query_functions_ =  std::move(change_functions);
} 