/* Copyright 2022 Stellar Development Foundation and contributors. Licensed
   under the Apache License, Version 2.0. See the COPYING file at the root
   of this distribution or at http://www.apache.org/licenses/LICENSE-2.0 */
%skeleton "lalr1.cc" /* -*- C++ -*- */
%require "3.0.4"

%code requires
{
#include "util/xdrquery/XDRQueryError.h"
#include "util/xdrquery/XDRQueryEval.h"

#include <memory>
}

%code provides
{
#define YY_DECL xdrquery::XDRQueryParser::symbol_type yylex()
YY_DECL;

namespace xdrquery
{
XDRQueryStatement
parseXDRQuery(std::string const& query);
}  // namespace xdrquery
}

%define api.value.type variant
%define parser_class_name { XDRQueryParser }
%define api.namespace { xdrquery }
%define api.token.prefix {TOKEN_}
%define api.token.constructor

%parse-param { XDRQueryStatement& root }

%token <std::string> ID
%token <std::string> INT
%token <std::string> STR

%token NULL
%token SUM
%token AVG
%token COUNT

%token AND "&&"
%token OR "||"

%token EQ "=="
%token NE "!="
%token GT ">"
%token GE ">="
%token LT "<"
%token LE "<="

%token LPAREN "("
%token RPAREN ")"

%token DOT "."
%token COMMA ","

%token END 0

%left "||"
%left "&&"
%left "==" "!=" ">" ">=" "<" "<="

%type <std::shared_ptr<EvalNode>> literal operand
%type <std::shared_ptr<BoolEvalNode>> comparison_expr logic_expr
%type <std::shared_ptr<FieldNode>> field

%type <std::shared_ptr<Accumulator>> accumulator
%type <std::shared_ptr<AccumulatorList>> accumulator_list

%type <std::shared_ptr<FieldList>> field_list

%%

statement: logic_expr { root = std::move($1); }
         | accumulator_list { root = std::move($1); }
         | field_list { root = std::move($1); }

logic_expr: comparison_expr { $$ = std::move($1); }
          | "(" logic_expr ")" { $$ = std::move($2); }
          | logic_expr "&&" logic_expr {
            $$ = std::make_shared<BoolOpNode>(BoolOpNodeType::AND,
                std::move($1), std::move($3)); }
          | logic_expr "||" logic_expr {
            $$ = std::make_shared<BoolOpNode>(BoolOpNodeType::OR,
                std::move($1), std::move($3)); }

comparison_expr: operand "==" operand {
        $$ = std::make_shared<ComparisonNode>(ComparisonNodeType::EQ,
                std::move($1), std::move($3)); }
    | operand "!=" operand {
        $$ = std::make_shared<ComparisonNode>(ComparisonNodeType::NE,
                std::move($1), std::move($3)); }
    | operand "<" operand {
        $$ = std::make_shared<ComparisonNode>(ComparisonNodeType::LT,
                std::move($1), std::move($3)); }
    | operand "<=" operand {
        $$ = std::make_shared<ComparisonNode>(ComparisonNodeType::LE,
                std::move($1), std::move($3)); }
    | operand ">" operand {
        $$ = std::make_shared<ComparisonNode>(ComparisonNodeType::GT,
                std::move($1), std::move($3)); }
    | operand ">=" operand {
        $$ = std::make_shared<ComparisonNode>(ComparisonNodeType::GE,
                std::move($1), std::move($3)); }

operand: literal { $$ = std::move($1); }
       | field { $$ = std::move($1); }

literal: INT { $$ = std::make_shared<LiteralNode>(LiteralNodeType::INT, $1); }
       | STR { $$ = std::make_shared<LiteralNode>(LiteralNodeType::STR, $1); }
       | NULL { $$ = std::make_shared<LiteralNode>(LiteralNodeType::NULL_LITERAL, ""); }

field: ID { $$ = std::make_shared<FieldNode>($1); }
     | field "." ID { $1->mFieldPath.push_back($3); $$ = std::move($1); }


accumulator_list: accumulator { $$ = std::make_shared<AccumulatorList>(std::move($1)); }
                | accumulator_list "," accumulator { $1->addAccumulator($3); $$ = std::move($1); }

accumulator: COUNT "(" ")" { 
                $$ = std::make_shared<Accumulator>(AccumulatorType::COUNT); }
           | SUM "(" field ")" {
                $$ = std::make_shared<Accumulator>(
                    AccumulatorType::SUM, std::move($3));
           }
           | AVG "(" field ")" {
                $$ = std::make_shared<Accumulator>(
                    AccumulatorType::AVERAGE, std::move($3));
           }

field_list: field { $$ = std::make_shared<FieldList>(std::move($1)); }
          | field_list "," field { $1->addField($3); $$ = std::move($1); }

%%

#ifdef __has_feature
    #if __has_feature(address_sanitizer)
        #define ASAN_ENABLED
    #endif
#else
    #ifdef __SANITIZE_ADDRESS__
        #define ASAN_ENABLED
    #endif
#endif    

#ifdef ASAN_ENABLED
#include <sanitizer/lsan_interface.h>   
#endif

void beginScan(char const* s);
void endScan();

namespace xdrquery
{
void
XDRQueryParser::error(std::string const& error)
{
    throw XDRQueryError("Parsing error: '" + error + "'.");
}

XDRQueryStatement
parseXDRQuery(std::string const& query)
{
    // LeakSantizer (likely) incorrectly identifies some small leaks in 
    // lexer, hence disable it for the query parsing. According
    // to the docs, calling `yylex_destoy` should be enough to do the proper
    // cleanup.
#ifdef ASAN_ENABLED
    __lsan_disable();
#endif    
    beginScan(query.c_str());
    XDRQueryStatement root;
    XDRQueryParser parser(root);
    parser.parse();
    endScan();
#ifdef ASAN_ENABLED
    __lsan_enable();
#endif
    return root;
}
}  // namespace xdrquery
