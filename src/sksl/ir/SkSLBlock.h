/*
 * Copyright 2016 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef SKSL_BLOCK
#define SKSL_BLOCK

#include "src/sksl/ir/SkSLStatement.h"
#include "src/sksl/ir/SkSLSymbolTable.h"

namespace SkSL {

/**
 * A block of multiple statements functioning as a single statement.
 */
class Block : public Statement {
public:
    static constexpr Kind kStatementKind = Kind::kBlock;

    Block(int offset, StatementArray statements,
          const std::shared_ptr<SymbolTable> symbols = nullptr, bool isScope = true)
    : INHERITED(offset, kStatementKind, BlockData{std::move(symbols), isScope},
                std::move(statements)) {}

    const StatementArray& children() const {
        return fStatementChildren;
    }

    StatementArray& children() {
        return fStatementChildren;
    }

    bool isScope() const {
        return this->blockData().fIsScope;
    }

    void setIsScope(bool isScope) {
        this->blockData().fIsScope = isScope;
    }

    std::shared_ptr<SymbolTable> symbolTable() const {
        return this->blockData().fSymbolTable;
    }

    bool isEmpty() const override {
        for (const std::unique_ptr<Statement>& stmt : this->children()) {
            if (!stmt->isEmpty()) {
                return false;
            }
        }
        return true;
    }

    std::unique_ptr<Statement> clone() const override {
        StatementArray cloned;
        cloned.reserve_back(this->children().size());
        for (const std::unique_ptr<Statement>& stmt : this->children()) {
            cloned.push_back(stmt->clone());
        }
        return std::make_unique<Block>(fOffset, std::move(cloned),
                                       SymbolTable::WrapIfBuiltin(this->symbolTable()),
                                       this->isScope());
    }

    String description() const override {
        String result("{");
        for (const std::unique_ptr<Statement>& stmt : this->children()) {
            result += "\n";
            result += stmt->description();
        }
        result += "\n}\n";
        return result;
    }

private:
    using INHERITED = Statement;
};

}  // namespace SkSL

#endif
