#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_goods_data.py — 生成 goods 表的建表语句 + 随机数据 INSERT 语句。

目标：产生约 6~9 个 segment 的数据量。
  kMaxSegmentRowCount = 65536 (src/storage/datastructs.h)
  默认 500000 行 => 7 个满 segment + 1 个部分 segment = 8 个 segment。

用法：
  python3 tools/gen_goods_data.py [rows] > goods.sql
  ./build/bin/simple_olap < goods.sql

注意（受当前 parser/binder 限制，生成器刻意遵守）：
  - 数值字面量全部为正（parser 不支持负号开头的一元表达式）
  - DOUBLE 值必须带小数点（INTEGER token 会被解析成 int32，无法绑定 DOUBLE 列）
  - 不使用 BIGINT / FLOAT 列（INT64 字面量、double->FLOAT 绑定均不支持）
  - 字符串用单引号，长度 <= 60 字节（VARCHAR 内联槽上限）
  - 关键字大写
"""

import random
import sys

ROWS = int(sys.argv[1]) if len(sys.argv) > 1 else 500000
BATCH = 2000          # 每个 INSERT 语句的行数，控制单条语句的 AST 大小
SEED = 20260909

random.seed(SEED)

CATEGORIES = [
    "electronics", "books", "toys", "sports", "kitchen",
    "garden", "office", "beauty", "auto", "pets",
]
BRANDS = [
    "acme", "bright", "corex", "deltal", "everg",
    "finch", "glow", "helio", "ivory", "juno",
]

def rand_name(i: int) -> str:
    return f"{random.choice(BRANDS)}-item-{i:07d}"

def emit_header():
    print("CREATE TABLE goods (")
    print("    id INT,")
    print("    name VARCHAR,")
    print("    category VARCHAR,")
    print("    price DOUBLE,")
    print("    stock INT,")
    print("    rating DOUBLE")
    print(");")

def fmt_row(i: int) -> str:
    # id 单调递增：让 zone-map 在 id 谓词上有实际剪枝效果
    gid = i + 1
    name = rand_name(gid)
    category = random.choice(CATEGORIES)
    # price: 1.00 ~ 9999.99，两位小数，必须带小数点
    price = f"{random.uniform(1.0, 9999.99):.2f}"
    # stock: 0 ~ 9999（非负，INT）
    stock = random.randint(0, 9999)
    # rating: 0.50 ~ 5.00，必须带小数点
    rating = f"{random.uniform(0.5, 5.0):.2f}"
    return f"({gid}, '{name}', '{category}', {price}, {stock}, {rating})"

def main():
    emit_header()
    for start in range(0, ROWS, BATCH):
        end = min(start + BATCH, ROWS)
        rows = ",\n    ".join(fmt_row(i) for i in range(start, end))
        print(f"INSERT INTO goods VALUES\n    {rows};")

if __name__ == "__main__":
    main()
