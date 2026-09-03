#pragma once

// 此文件原为模块测试用的临时 SQL 定义，现已全部迁移：
//   - AggType                -> src/type.h（公共层）
//   - CreateTableStatement / InsertStatement / SelectStatement /
//     SelectItem / Expression / Statement -> src/sql/ast/（正式 AST）
//   - SelectTarget / ExprTarget / SelectTargetStatement
//                            -> src/storage/scan_request.h（存储层正式接口）
//
// 保留此头文件仅为过渡期兼容旧 include 路径，后续应删除。

#include "../sql/ast/statement.h"
#include "scan_request.h"
