/* Copyright 2022 Stellar Development Foundation and contributors. Licensed
   under the Apache License, Version 2.0. See the COPYING file at the root
   of this distribution or at http://www.apache.org/licenses/LICENSE-2.0 */
%top{
#ifdef _MSC_VER
#include <stdint.h>
#include <io.h>
#define popen _popen
#define pclose _pclose
#define access _access
#define isatty _isatty
#define fileno _fileno
#endif
}
%{
#ifndef _MSC_VER
#include <unistd.h>
#define register
#endif

#include "util/xdrquery/XDRQueryParser.h"
%}

%option noyywrap
%option nounput noinput
%option batch

IDENTIFIER  [a-zA-Z_][a-zA-Z_0-9]*
INT -?[0-9]+
STRING (\"[^"\n]*\")|('[^'\n]*')
WHITESPACE [ \t\r\n]

%%

NULL  { return xdrquery::XDRQueryParser::make_NULL(); }

sum { return xdrquery::XDRQueryParser::make_SUM(); }
avg { return xdrquery::XDRQueryParser::make_AVG(); }
count { return xdrquery::XDRQueryParser::make_COUNT(); }
entry_size { return xdrquery::XDRQueryParser::make_ENTRY_SIZE(); }

{IDENTIFIER}  { return xdrquery::XDRQueryParser::make_ID(yytext); }
{INT}         { return xdrquery::XDRQueryParser::make_INT(yytext); }

"&&"  { return xdrquery::XDRQueryParser::make_AND(); }
"||"  { return xdrquery::XDRQueryParser::make_OR(); }

"=="  { return xdrquery::XDRQueryParser::make_EQ(); }
"!="  { return xdrquery::XDRQueryParser::make_NE(); }
">="  { return xdrquery::XDRQueryParser::make_GE(); }
">"   { return xdrquery::XDRQueryParser::make_GT(); }
"<="  { return xdrquery::XDRQueryParser::make_LE(); }
"<"   { return xdrquery::XDRQueryParser::make_LT(); }

"("   { return xdrquery::XDRQueryParser::make_LPAREN(); }
")"   { return xdrquery::XDRQueryParser::make_RPAREN(); }

"."   { return xdrquery::XDRQueryParser::make_DOT(); }
","   { return xdrquery::XDRQueryParser::make_COMMA(); }

{STRING} { 
    std::string s(yytext + 1); 
    s.pop_back();
    return xdrquery::XDRQueryParser::make_STR(s);
}

{WHITESPACE}+ /* discard */;

<<EOF>> { return xdrquery::XDRQueryParser::make_END(); }

.     { throw xdrquery::XDRQueryParser::syntax_error("Unexpected character: " + std::string(yytext)); }

%%

void beginScan(char const* s) {
  yy_scan_string(s);
}

void endScan() {
  yylex_destroy();
}
