%{
#include <stdio.h>
#include "vsiParser.tab.h"
%}
%option yylineno
%%
VSI_DEF_BRANCH        {return VSI_TOK_BRANCH;}
VSI_DEF_INST_BRANCH   {return VSI_TOK_INST_BRANCH;}
VSI_DEF_LEAF          {return VSI_TOK_LEAF;}
VSI_DEF_INST_LEAF     {return VSI_TOK_INST_LEAF;}
VSI_DEF_TYPE          {return VSI_TOK_TYPE;}
VSI_DEF_ARRAY         {return VSI_TOK_ARRAY;}
VSI_DEF_STRUCT        {return VSI_TOK_STRUCT;}
VSI_DEF_STRUCT_FIELD  {return VSI_TOK_STRUCT_FIELD;}
[0-9]+               {yylval = strdup(yytext); return(VSI_TOK_INTEGER);}
[_a-zA-Z][_a-zA-Z0-9]*  {yylval = strdup(yytext); return VSI_TOK_WORD;}
\"[^"]*\"            {yylval = strdup(yytext); return VSI_TOK_QUOTEDSTR; }
,                    {return ',';}
\(                   {return '(';}
\)                   {return ')';}
;                    {return ';';} 
\{                   {return '{';}
\}                   {return '}';}
[ \t\f\v\r]
\n
.
%%

