%{

/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>

#include "vm_basic_types.h"
#include "vm_basic_defs.h"

#include "vsiDefs.h"

#define YYERROR_VERBOSE 1
#define VSI_NODEDEFENTRIES_ALLOC         100
#define VSI_TYPEENTRIES_ALLOC        10
#define VSI_STRUCTENTRIES_ALLOC      10
#define VSI_STRUCTFIELDENTRIES_ALLOC 100
#define VSI_MAX_LOOKUPTAB            5000

typedef struct _VSINodeDefEntry {
   Bool  isLeaf;
   Bool  isInstance;
   char  *name;
   char  *parentName;
   char  *helpStr;
   char  *listFunction;
   char  *getFunction;
   char  *setFunction;
   char  *outputType;
} VSINodeDefTabEntry;

typedef struct _VSINodeLookupTabEntry {
   Bool    valid;
   uint32  child;
   uint32  nextSibling;
} VSINodeLookupTabEntry;

typedef struct _VSIStructFieldTabEntry {
   char    *fieldName;
   uint32  fieldTypeDefIdx;
   char    *helpStr;
} VSIStructFieldTabEntry;

typedef struct _VSITypeDefTabEntry {
   char    *typeName;
   char    *helpStr;
   char    *baseType;

   VSITypeDefType type;

   union { 
      struct VSITypeDefTabEntryArray {
         uint32 size;
      } arrayType;

      struct VSITypeDefTabEntryStruct {
         uint32  nFields;
         VSIStructFieldTabEntry *fieldEnts;
      } structType;
   } u;

} VSITypeDefTabEntry;

typedef struct _VSITab {
   uint32  tabSize;        /* table size */
   uint32  nEnts;          /* number of populated entries in table */
   uint32  entSize;        /* sizeof( entry ) */
   uint32  allocQuantum;   /* malloc quantum to resize table */
   void    *entries;       /* array of table entries */
} VSITab;

static uint32  curStructFieldsCount;
static uint32  curStructFieldsStartIdx; 

static VSITab vsiNodeDefTab;
static VSITab vsiLookupTab;
static VSITab vsiTypeDefTab;
static VSITab vsiStructFieldTab;

static char *inputFileName = "<STDIN>";
static char *curQuotedStr = NULL;
static int errorCount;

static struct option longOptions[] = {
   { "enums", 0, 0, 'e' },
   { "lookup", 0, 0, 'l' },
   { "funcs", 0, 0, 'f' },
   { "bolier-plate", 0, 0, 'b' },
   { "prologue", 0, 0, 'p' },
   { "typedefs", 0, 0, 't' },
   { 0, 0, 0, 0 },
};

extern int yylineno;
void 
yyerror(const char *str)
{
   fprintf(stderr, "error: %s @ %s:%d\n", str, inputFileName, yylineno);
   errorCount++;
}

int 
yywrap()
{
   return 1;
}


/*
 *----------------------------------------------------------------------
 *
 * VSITypeDefType2Str --
 *   Convert VSITypeDefType to ASCIIZ
 *
 * Results:
 *    Printable string
 *
 * Side effects:
 *    None
 *  
 *----------------------------------------------------------------------
 */
static char *
VSITypeDefType2Str(VSITypeDefType type)
{
   switch (type) {
   case VSITypeBase:
      return "VSITypeBase";
      break;

   case VSITypeArray:
      return "VSITypeArray";
      break;

   case VSITypeStruct:
      return "VSITypeStruct";
      break;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * VSIInitTable --
 *   Init table
 *
 * Results:
 *    None 
 *
 * Side effects:
 *    None
 *  
 *----------------------------------------------------------------------
 */
static void 
VSIInitTable(VSITab *tab, uint32 entSize, uint32 allocQuantum)
{
   tab->entSize = entSize;
   tab->tabSize = tab->allocQuantum = allocQuantum;
   tab->nEnts = 0;
   tab->entries = malloc(tab->entSize * tab->tabSize);
   if (!tab->entries) {
      fprintf(stderr, "VSIInitTable: malloc failed\n");
      exit (1);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * VSIReAllocTable --
 *   Grow table by allocQuantum
 *
 * Results:
 *    None 
 *
 * Side effects:
 *    None
 *  
 *----------------------------------------------------------------------
 */
static void
VSIReAllocTable(VSITab *tab)
{
   tab->tabSize += tab->allocQuantum;
   tab->entries = realloc(tab->entries, tab->entSize * tab->tabSize);
   if (!tab->entries) {
      fprintf(stderr, "VSIReAllocTable: realloc failed\n");
      exit(1);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * VSIConcatStr --
 *   Concatenate and form a new string from two input strings
 *
 * Results:
 *    Concatenated string
 *
 * Side effects:
 *    Orignal strings remain intact, caller must free them if not needed
 *  
 *----------------------------------------------------------------------
 */
static char *
VSIConcatStr(char *str1, char *str2)
{
   int str1len = strlen(str1);
   int str2len = strlen(str2);

   char *newstr = (char *)calloc(str1len + str2len + 1, sizeof(char));
   if (!newstr) {
      fprintf(stderr, "VSIConcatStr: calloc failed\n");
      exit(1);
   }

   strcat(newstr, str1);
   strcat(newstr, str2);
   return newstr;
}

/*
 *----------------------------------------------------------------------
 *
 * VSIAddChild --
 *    Add new child node to parent branch
 *
 * Results:
 *    None
 *
 * Side effects:
 *    None
 *  
 *----------------------------------------------------------------------
 */
static void
VSIAddChild(VSITab *nodeLookupTab, VSITab *nodeDefTab, 
                 uint32 parent, uint32 newChild)
{
   int i;
   uint32 child, curSibling;
   VSINodeDefTabEntry *def = (VSINodeDefTabEntry *)nodeDefTab->entries;
   VSINodeLookupTabEntry *lent = (VSINodeLookupTabEntry *)nodeLookupTab->entries;

   if (!lent[parent].child) {
      lent[parent].child = newChild;
   }
   else {
      lent[newChild].nextSibling = lent[parent].child;
      lent[parent].child = newChild;
   }

   lent[newChild].valid = TRUE;
   lent[newChild].child = 0;

   nodeLookupTab->nEnts++;
}

/*
 *----------------------------------------------------------------------
 *
 * VSIBuildBranch --
 *    Scan definitions table for child nodes of parent branch and link
 *    them
 *
 * Results:
 *    None
 *
 * Side effects:
 *    None
 *  
 *----------------------------------------------------------------------
 */
static void
VSIBuildBranch(VSITab *nodeLookupTab, VSITab *nodeDefTab, unsigned int branchEnt)
{
   int i;
   uint32 nextSibling;
   VSINodeDefTabEntry *def = (VSINodeDefTabEntry *)nodeDefTab->entries;
   VSINodeLookupTabEntry *lent = (VSINodeLookupTabEntry *)
                                      nodeLookupTab->entries;

   /*
    * Scan entries for matching parent name
    */
   for (i=1; i < nodeDefTab->nEnts; i++) {
      if (!strcmp(def[i].parentName, def[branchEnt].name)) {
            /*
             * Make sure it's not a duplicate
             */
            if (lent[branchEnt].child) {
               nextSibling = lent[branchEnt].child;
               while (nextSibling) {
                  if (!strcmp(def[i].name, def[nextSibling].name)) {
                     fprintf(stderr, "duplicate child node \"%s\" for root "
                         "node \"%s\"\n", def[i].name, def[branchEnt].name);
                     exit(1);
                  }
                  nextSibling = lent[nextSibling].nextSibling;
               }
            }

            /*
             * Add new child node
             */
            VSIAddChild(nodeLookupTab, nodeDefTab, branchEnt, i);
      }
   }

   /*
    * Recursively build subtree under children
    */
   if (!def[branchEnt].isLeaf && lent[branchEnt].child) {
        nextSibling = lent[branchEnt].child;
	while (nextSibling) {
           VSIBuildBranch(nodeLookupTab, nodeDefTab, nextSibling);
           nextSibling = lent[nextSibling].nextSibling;
        }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * VSIBuildLookupTable --
 *    Build lookup table from definitions table
 *
 * Results:
 *    None 
 *
 * Side effects:
 *    None
 *  
 *----------------------------------------------------------------------
 */
static void
VSIBuildLookupTable(VSITab *nodeLookupTab, VSITab *nodeDefTab)
{
   VSIBuildBranch(nodeLookupTab, nodeDefTab, 0);
}

/*
 *----------------------------------------------------------------------
 *
 * VSISearchTypeDef -- 
 *    Search typedefs table for previously defined types
 *
 * Results:
 *    Index of the typedef in table on success; -1 if not found
 *
 * Side effects:
 *    None
 *  
 *----------------------------------------------------------------------
 */
static int
VSISearchTypeDef(VSITab *typeDefTab, char *type)
{
   int i;
   VSITypeDefTabEntry *typeDef = (VSITypeDefTabEntry *)typeDefTab->entries;


   /*
    * TODO: Eventually this may have to become a hash table for faster compiles
    */
   for (i=0; i < typeDefTab->nEnts; i++) {
      if (!strcmp(type, typeDef[i].typeName)) {
         return i;
      }
   }

   return -1;
}

/*
 *----------------------------------------------------------------------
 *
 * VSIInitNodeDefTable --
 *    Initialize node definitions table
 *
 * Results:
 *    None
 *
 * Side effects:
 *    None
 *  
 *----------------------------------------------------------------------
 */
static void
VSIInitNodeDefTable(VSITab *nodeDefTab)
{
   VSINodeDefTabEntry *ndef;

   VSIInitTable(nodeDefTab, sizeof(VSINodeDefTabEntry), 
                                              VSI_NODEDEFENTRIES_ALLOC);

   ndef = (VSINodeDefTabEntry *)nodeDefTab->entries;

   /*
    * Init root entry
    */   
   nodeDefTab->nEnts = 1;

   ndef[0].isLeaf = FALSE;
   ndef[0].isInstance = FALSE;
   ndef[0].name = "root";
   ndef[0].parentName = "root";
   ndef[0].helpStr = "\"root\"";
   ndef[0].listFunction = NULL;
   ndef[0].getFunction = NULL;
   ndef[0].setFunction = NULL;
   ndef[0].outputType = NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * VSIInitNodeLookupTable --
 *    Initialize node lookup table
 *
 * Results:
 *    None
 *
 * Side effects:
 *    None
 *  
 *----------------------------------------------------------------------
 */
static void 
VSIInitNodeLookupTable(VSITab *nodeLookupTab, VSITab *nodeDefTab)
{
   VSINodeLookupTabEntry *rootEnt;

   VSIInitTable(nodeLookupTab, sizeof(VSINodeLookupTabEntry), nodeDefTab->nEnts);

   rootEnt = (VSINodeLookupTabEntry *)nodeLookupTab->entries;
   rootEnt->valid = TRUE;
   rootEnt->child = 0;
}

/*
 *----------------------------------------------------------------------
 *
 * VSIInitTypeDefTable --
 *    Initialize typedef table
 *
 * Results:
 *    None
 *
 * Side effects:
 *    None
 *  
 *----------------------------------------------------------------------
 */
static void 
VSIInitTypeDefTable(VSITab *typeDefTab)
{
   VSIInitTable(typeDefTab, sizeof(VSITypeDefTabEntry), VSI_TYPEENTRIES_ALLOC);
}

/*
 *----------------------------------------------------------------------
 *
 * VSIInitStructFieldTab --
 *    Initialize structure fields table
 *
 * Results:
 *    None
 *
 * Side effects:
 *    None
 *  
 *----------------------------------------------------------------------
 */
static void 
VSIInitStructFieldTab(VSITab *structFieldTab)
{
   VSIInitTable(structFieldTab, sizeof(VSIStructFieldTabEntry),
                                        VSI_STRUCTFIELDENTRIES_ALLOC);
}

/*
 *----------------------------------------------------------------------
 *
 * VSIPrintEnums --
 *    Print enums table
 *
 * Results:
 *    None
 *
 * Side effects:
 *    Prints enums table to stdout
 *  
 *----------------------------------------------------------------------
 */
static void
VSIPrintEnums(VSITab *nodeDefTab)
{
   int i;
   VSINodeDefTabEntry *def = (VSINodeDefTabEntry *)nodeDefTab->entries;

   printf("\n#define VSIDEF_VSINODEID 1\n\n");

   printf("\ntypedef enum {\n");
   for (i=0; i < nodeDefTab->nEnts; i++) {
      printf("   %s%s,\n", "VSI_NODE_", def[i].name);
   }
   printf("   %s\n", "VSI_NODE_MAX");
   printf("} VSI_NodeID;\n");
}

/*
 *----------------------------------------------------------------------
 *
 * VSIPrintBoilerPlateStart --
 *    Print starting #defs to prevent double inclusion of header files
 *
 * Results:
 *    None
 *
 * Side effects:
 *    None
 *  
 *----------------------------------------------------------------------
 */
static void
VSIPrintBoilerPlateStart(char *bpName)
{
   printf("#ifndef %s\n", bpName);
   printf("#define %s\n\n\n", bpName);
}

/*
 *----------------------------------------------------------------------
 *
 * VSIPrintBoilerPlateEnd --
 *    Print ending #defs to prevent double inclusion of header files
 *
 * Results:
 *    None
 *
 * Side effects:
 *    None
 *  
 *----------------------------------------------------------------------
 */
static void
VSIPrintBoilerPlateEnd(char *bpName)
{
   printf("\n\n#endif //%s\n", bpName);
}

/*
 *----------------------------------------------------------------------
 *
 * VSIPrintFilePrologue --
 *    Print headers, includes, defs etc.
 *
 * Results:
 *    None
 *
 * Side effects:
 *    None
 *  
 *----------------------------------------------------------------------
 */
static void
VSIPrintFilePrologue()
{
   printf("#include \"vm_basic_types.h\"\n");
   printf("#include \"vm_basic_defs.h\"\n");
   printf("#include \"vm_assert.h\"\n");
   printf("#include \"gen_vmksysinfodefs.h\"\n");
   printf("#include \"vsiDefs.h\"\n");
   printf("#define VSITREE_ALLOW_INCLUDE\n");
   printf("#include \"vsiTree_int.h\"\n\n");
}

/*
 *----------------------------------------------------------------------
 *
 * VSIPrintHandlers --
 *    Print handler functions
 *
 * Results:
 *    None
 *
 * Side effects:
 *    Prints handler functions to stdout
 *  
 *----------------------------------------------------------------------
 */
static void
VSIPrintHandlers(VSITab *nodeDefTab)
{
   int i;
   VSINodeDefTabEntry *def = (VSINodeDefTabEntry *)nodeDefTab->entries;

   printf("\nVSI_Handlers vsiHandlers[] = {\n");
   for (i=0; i < nodeDefTab->nEnts; i++) {

      printf("   {   /* %s%s */\n"
             "       /* ListHandler */   %s%s, \n"
             "       /* GetHandler  */   (VSI_GetHandler)%s%s, \n"
             "       /* SetHandler  */   %s%s  \n"
             "   },\n",
            "VSI_NODE_", def[i].name,
            def[i].listFunction ? "&": "",
            def[i].listFunction ? def[i].listFunction : "NULL",
            def[i].getFunction ? "&" : "",
            def[i].getFunction ? def[i].getFunction : "NULL",
            def[i].setFunction ? "&" : "",
            def[i].setFunction ? def[i].setFunction : "NULL");
   }
   printf("};\n");
}

/*
 *----------------------------------------------------------------------
 *
 * VSIPrintNodeLookupTable --
 *    Print nodes lookup table
 *
 * Results:
 *    None
 *
 * Side effects:
 *    Prints nodes lookup table to stdout
 *  
 *----------------------------------------------------------------------
 */
static void
VSIPrintNodeLookupTable(VSITab *nodeLookupTab, VSITab *nodeDefTab, Bool printTypeDefTab)
{
   int i, typeIdx;
   VSINodeLookupTabEntry *lent = (VSINodeLookupTabEntry *)nodeLookupTab->entries;
   VSINodeDefTabEntry *def = (VSINodeDefTabEntry *)nodeDefTab->entries;
   VSITab *typeDefTab = &vsiTypeDefTab;

   printf("\nuint32 vsiMaxNodes = %d;\n", nodeDefTab->nEnts);

   printf("\nVSI_NodeInfo vsiNodesLookupTab[] = {\n");
   for (i=0; i < nodeDefTab->nEnts; i++) {

      if (def[i].outputType) {
         typeIdx = VSISearchTypeDef(typeDefTab, def[i].outputType);
         if (typeIdx == -1) {
            yyerror("unknown output type");
            exit(1);
         }
      }

      printf("   {   /* nodeID      */   %s%s,\n"
             "       /* nodeName    */   \"%s\",\n"
             "       /* isLeaf      */   %s, \n"
             "       /* isInstance  */   %s, \n"
             "       /* parent      */   %s%s, \n"
             "       /* nextSibling */   %s%s, \n"
             "       /* firstChild  */   %s%s, \n"
             "       /* outputType  */   %s%d%s \n"
             "   },\n",
             "VSI_NODE_", def[i].name,
             def[i].name,
             def[i].isLeaf ? "TRUE" : "FALSE",
             def[i].isInstance ? "TRUE" : "FALSE",
             "VSI_NODE_", def[i].parentName,
             "VSI_NODE_", def[lent[i].nextSibling].name,
             "VSI_NODE_", def[lent[i].child].name,
             printTypeDefTab && def[i].outputType ? 
                                       "&vsiTypeDefsLookupTab[" : "",
             printTypeDefTab && def[i].outputType ?  typeIdx : 0,
             printTypeDefTab && def[i].outputType ? "]" : "");
   }
   printf("};\n");
}

/*
 *----------------------------------------------------------------------
 *
 * VSIPrintTypeDefTable --
 *    Print struct lookup table
 *
 * Results:
 *    None
 *
 * Side effects:
 *    Prints struct lookup table to stdout
 *  
 *----------------------------------------------------------------------
 */
static void
VSIPrintTypeDefTable(VSITab *structFieldTab, VSITab *typeDefTab)
{
   int i;
   VSIStructFieldTabEntry *structFields = 
                           (VSIStructFieldTabEntry *)structFieldTab->entries;
   VSITypeDefTabEntry *typeDef = (VSITypeDefTabEntry *)typeDefTab->entries;

   char *arrayNElementStr = ".u.array_t.nElement = ";
   char *structNFieldStr = ".u.struct_t.nStructField = ";
   char *structFieldsStr = ".u.struct_t.structFields = ";   
   char arrayNElement[10];
   char structNField[10];

   printf("\nVSI_TypeDef vsiTypeDefsLookupTab[] = {\n");
   for (i=0; i < typeDefTab->nEnts; i++) {

      snprintf(arrayNElement, 10, "%d", typeDef[i].u.arrayType.size);
      snprintf(structNField, 10, "%d", typeDef[i].u.structType.nFields);

      printf("   {   /* name             */   \"%s\",\n"
             "       /* type             */   %s,\n"
             "       /* size             */   sizeof(%s),\n"
             "       /* helpStr          */   %s,\n"
             "       %s%s%s"
             "       %s%s%s%s"
             "   },\n",

             typeDef[i].typeName,
             VSITypeDefType2Str(typeDef[i].type),
             typeDef[i].typeName,
             typeDef[i].helpStr ? typeDef[i].helpStr : "NULL",

             typeDef[i].type == VSITypeArray  ?  arrayNElementStr :
             typeDef[i].type == VSITypeStruct ?  structNFieldStr : "",

             typeDef[i].type == VSITypeArray  ?  arrayNElement :
             typeDef[i].type == VSITypeStruct ?  structNField : "",

             typeDef[i].type == VSITypeArray  ?  ",\n" :
             typeDef[i].type == VSITypeStruct ?  ",\n" : "\n",

             typeDef[i].type == VSITypeStruct ?  structFieldsStr : "",
             typeDef[i].type == VSITypeStruct ?  typeDef[i].typeName : "",
             typeDef[i].type == VSITypeStruct ?  "Fields" : "",
             typeDef[i].type == VSITypeStruct ?  ",\n" : "\n");
   }
  printf("};\n");
}

/*
 *----------------------------------------------------------------------
 *
 * VSIPrintStructFieldTable --
 *    Print struct fields lookup table
 *
 * Results:
 *    None
 *
 * Side effects:
 *    Prints struct fields lookup table to stdout
 *  
 *----------------------------------------------------------------------
 */
static void
VSIPrintStructFieldTable(VSITab *typeDefTab, VSITab *structFieldTab)
{
   int i, j;
   char *structName;
   uint32 nFields;
   VSIStructFieldTabEntry *fieldEnts;

   VSITypeDefTabEntry *typeDef = (VSITypeDefTabEntry *)typeDefTab->entries;

   for (i=0; i < typeDefTab->nEnts; i++) {

      if (typeDef[i].type != VSITypeStruct) {
         continue;
      }

      structName = typeDef[i].typeName;
      nFields = typeDef[i].u.structType.nFields;
      fieldEnts = typeDef[i].u.structType.fieldEnts;

      printf("\nVSI_StructField %sFields[] = {\n", structName);
      for (j=0; j < nFields; j++) {
         printf("   {   /* fieldName       */   \"%s\",\n"
                "       /* fieldType       */   &vsiTypeDefsLookupTab[%d],\n"
                "       /* fieldOffset     */   offsetof(%s, %s),\n"
                "       /* helpStr         */   %s,\n"
                "   },\n",
                fieldEnts[j].fieldName,
                fieldEnts[j].fieldTypeDefIdx,
                structName, fieldEnts[j].fieldName,
                fieldEnts[j].helpStr);
      }
      printf("};\n");
   }
}

static void
VSIUsage()
{
   fprintf(stderr, "Usage: vsiParser -e -l -f\n");
}
extern FILE *yyin;

int
main(int argc, char **argv)
{
   int c ;
   int optionIndex = 0;
   char *fname = NULL;

   Bool printEnums = FALSE;
   Bool printBoilerPlate = FALSE;
   Bool printNodeLookupTab = FALSE;
   Bool printLookupFuncs = FALSE;
   Bool printPrologue = FALSE;
   Bool printTypeDefTab = FALSE;

   VSITab *nodeDefTab = &vsiNodeDefTab;
   VSITab *typeDefTab = &vsiTypeDefTab;
   VSITab *nodeLookupTab = &vsiLookupTab;
   VSITab *structFieldTab = &vsiStructFieldTab;

   /*
    * Process commandline options
    */
   while (1) {
      c = getopt_long (argc, argv, "elfbptF:",
                       longOptions, &optionIndex);
      if (c == -1) {
         break;
      }

      switch (c) {
         case 'e':
            printEnums = TRUE;
            break;

         case 'l':
            printNodeLookupTab = TRUE;
            break;

         case 'f':
            printLookupFuncs = TRUE;
            break;

         case 'b':
            printBoilerPlate = TRUE;
            break;

         case 'p':
            printPrologue = TRUE;
            break;

         case 't':
            printTypeDefTab = TRUE;
            break;

         case 'F':
            fname = optarg;
            break;

         default:
            VSIUsage();
            break;
      }
   }

   /*
    * Init tables
    */
   VSIInitNodeDefTable(nodeDefTab);
   VSIInitTypeDefTable(typeDefTab);
   VSIInitStructFieldTab(structFieldTab);

   /*
    * Open input file
    */
   if (fname) {
      yyin = fopen(fname, "rt");
      if (!yyin) {
         fprintf(stderr, "Failed to open %s: %s\n", fname, strerror(errno));
         exit(1);
      }
      inputFileName = fname;
   }

   /*
    * Go parse!
    */
   yyparse();

   /*
    * Build parent-chlid relationships
    */
   VSIInitNodeLookupTable(nodeLookupTab, nodeDefTab);
   VSIBuildLookupTable(nodeLookupTab, nodeDefTab);
 
   /*
    * Print #ifndef/#def to prevent double inclusion of header file
    */
   if (printBoilerPlate) {
      VSIPrintBoilerPlateStart("_GEN_VMKSYSINFODEFS_");
   }

   /*
    * Print file headers, includes, defs etc.
    */
   if (printPrologue) {
      VSIPrintFilePrologue();
   }

   /*
    * Print struct description table
    */
   if (printTypeDefTab) {
      VSIPrintStructFieldTable(typeDefTab, structFieldTab);
      VSIPrintTypeDefTable(structFieldTab, typeDefTab);
   }

   /*
    * Print enums to stdout
    */
   if (printEnums) {
      VSIPrintEnums(nodeDefTab);
   }

   /*
    * Print lookup table to stdout
    */
   if (printNodeLookupTab) {
      VSIPrintNodeLookupTable(nodeLookupTab, nodeDefTab, printTypeDefTab);
   }

   /*
    * Print function handlers to stdout
    */
   if (printLookupFuncs) {
      VSIPrintHandlers(nodeDefTab);
   }

   if (printBoilerPlate) {
      VSIPrintBoilerPlateEnd("_GEN_VMKSYSINFODEFS_");
   }

   return errorCount;
}

%}
     
%token VSI_TOK_BRANCH VSI_TOK_INST_BRANCH VSI_TOK_LEAF VSI_TOK_INST_LEAF VSI_TOK_STRUCT VSI_TOK_TYPE VSI_TOK_ARRAY VSI_TOK_STRUCT_FIELD VSI_TOK_INTEGER VSI_TOK_WORD VSI_TOK_QUOTEDSTR 
     
%%


vsidef:  /* empty */ 
   | vsidef vsidefline
   ;

vsidefline: vsidef_branch
    | vsidef_inst_branch
    | vsidef_leaf
    | vsidef_inst_leaf
    | vsidef_type
    | vsidef_array
    | vsidef_struct
    | vsidef_integer_ignore
    | vsidef_word_ignore
    | vsidef_quotedstr_ignore
    | vsidef_openparen_ignore
    | vsidef_closeparen_ignore
    | vsidef_comma_ignore
    | vsidef_openbrace_ignore
    | vsidef_closebrace_ignore
    | vsidef_terminal_ignore
    ;


vsidef_branch:
   VSI_TOK_BRANCH '(' VSI_TOK_WORD ',' VSI_TOK_WORD ',' vsidef_quotedstr ')' ';'
   {
      VSITab *nodeDefTab = &vsiNodeDefTab;
      VSINodeDefTabEntry *nodeDef = (VSINodeDefTabEntry *)nodeDefTab->entries;
      int cur = nodeDefTab->nEnts;

      nodeDef[cur].isLeaf = FALSE;
      nodeDef[cur].isInstance = FALSE;
      nodeDef[cur].name = strdup((char *)$3);
      nodeDef[cur].parentName = strdup((char *)$5);
      nodeDef[cur].helpStr = strdup((char *)$7);

      nodeDefTab->nEnts++;
      if ( nodeDefTab->nEnts >= nodeDefTab->tabSize ) {
         VSIReAllocTable(nodeDefTab);
      }
   }
   ;

vsidef_inst_branch:
   VSI_TOK_INST_BRANCH '(' VSI_TOK_WORD ',' VSI_TOK_WORD ',' VSI_TOK_WORD ',' vsidef_quotedstr ')' ';'
   {
      VSITab *nodeDefTab = &vsiNodeDefTab;
      VSINodeDefTabEntry *nodeDef = (VSINodeDefTabEntry *)nodeDefTab->entries;
      int cur = nodeDefTab->nEnts;

      nodeDef[cur].isLeaf = FALSE;
      nodeDef[cur].isInstance = TRUE;
      nodeDef[cur].name = strdup((char *)$3);
      nodeDef[cur].parentName = strdup((char *)$5);
      if (strcmp((char *)$7, "NULL")) {
         nodeDef[cur].listFunction = strdup((char *)$7);
      }
      nodeDef[cur].helpStr = strdup((char *)$9);

      nodeDefTab->nEnts++;
      if ( nodeDefTab->nEnts >= nodeDefTab->tabSize ) {
         VSIReAllocTable(nodeDefTab);
      }
   }
   ;

vsidef_leaf:
   VSI_TOK_LEAF '(' VSI_TOK_WORD ',' VSI_TOK_WORD ',' VSI_TOK_WORD ',' VSI_TOK_WORD ',' VSI_TOK_WORD ',' vsidef_quotedstr ')' ';'
   {
      VSITab *nodeDefTab = &vsiNodeDefTab;
      VSINodeDefTabEntry *nodeDef = (VSINodeDefTabEntry *)nodeDefTab->entries;
      int cur = nodeDefTab->nEnts;


      nodeDef[cur].isLeaf = TRUE;
      nodeDef[cur].isInstance = FALSE;
      nodeDef[cur].name = strdup((char *)$3);
      nodeDef[cur].parentName = strdup((char *)$5);
      if (strcmp((char *)$5, "NULL")) {
         nodeDef[cur].getFunction = strdup((char *)$7);
      }
      if (strcmp((char *)$7, "NULL")) {
         nodeDef[cur].setFunction = strdup((char *)$9);
      }
      nodeDef[cur].outputType = strdup((char *)$11);
      nodeDef[cur].helpStr = strdup((char *)$13);

      nodeDefTab->nEnts++;
      if ( nodeDefTab->nEnts >= nodeDefTab->tabSize ) {
         VSIReAllocTable(nodeDefTab);
      }
   }
   ;

vsidef_inst_leaf:
   VSI_TOK_INST_LEAF '(' VSI_TOK_WORD ',' VSI_TOK_WORD ',' VSI_TOK_WORD ',' VSI_TOK_WORD ',' VSI_TOK_WORD ',' VSI_TOK_WORD ',' vsidef_quotedstr ')' ';'
   {
      VSITab *nodeDefTab = &vsiNodeDefTab;
      VSINodeDefTabEntry *nodeDef = (VSINodeDefTabEntry *)nodeDefTab->entries;
      int cur = nodeDefTab->nEnts;

      nodeDef[cur].isLeaf = TRUE;
      nodeDef[cur].isInstance = TRUE;
      nodeDef[cur].name = strdup((char *)$3);
      nodeDef[cur].parentName = strdup((char *)$5);

      if (strcmp((char *)$5, "NULL")) {
         nodeDef[cur].listFunction = strdup((char *)$7);
      }
      if (strcmp((char *)$7, "NULL")) {
         nodeDef[cur].getFunction = strdup((char *)$9);
      }
      if (strcmp((char *)$9, "NULL")) {
         nodeDef[cur].setFunction = strdup((char *)$11);
      }
      nodeDef[cur].outputType = strdup((char *)$13);
      nodeDef[cur].helpStr = strdup((char *)$15);

      nodeDefTab->nEnts++;
      if ( nodeDefTab->nEnts >= nodeDefTab->tabSize ) {
         VSIReAllocTable(nodeDefTab);
      }
   }
   ;

vsidef_type:
   VSI_TOK_TYPE '(' VSI_TOK_WORD ',' vsidef_typebase ',' vsidef_quotedstr ')' ';'
   {
      VSITab *typeDefTab = &vsiTypeDefTab;
      VSITypeDefTabEntry *typeDef = (VSITypeDefTabEntry *)typeDefTab->entries;
      int cur = typeDefTab->nEnts;

      typeDef[cur].type = VSITypeBase;
      typeDef[cur].typeName = strdup((char *)$3);
      typeDef[cur].helpStr = strdup((char *)$7);

      typeDefTab->nEnts++;
      if ( typeDefTab->nEnts >= typeDefTab->tabSize ) {
         VSIReAllocTable(typeDefTab);
      }
   }
   ;

vsidef_array:
   VSI_TOK_ARRAY '(' VSI_TOK_WORD ',' vsidef_typebase ',' VSI_TOK_INTEGER ')' ';'
   {
      VSITab *typeDefTab = &vsiTypeDefTab;
      VSITypeDefTabEntry *typeDef = (VSITypeDefTabEntry *)typeDefTab->entries;
      int cur = typeDefTab->nEnts;

      typeDef[cur].type = VSITypeArray;
      typeDef[cur].typeName = strdup((char *)$3);
      typeDef[cur].helpStr = NULL;

      typeDef[cur].u.arrayType.size = atoi((char *)$7);

      typeDefTab->nEnts++;
      if ( typeDefTab->nEnts >= typeDefTab->tabSize ) {
         VSIReAllocTable(typeDefTab);
      }
   }
   ;


vsidef_typebase:
   VSI_TOK_WORD vsidef_typebase_more
   {
      VSITab *typeDefTab = &vsiTypeDefTab;
      VSITypeDefTabEntry *typeDef = (VSITypeDefTabEntry *)typeDefTab->entries;
      int cur = typeDefTab->nEnts;

      // prepend
      if (typeDef[cur].baseType) {

         char *newBaseType = VSIConcatStr(VSIConcatStr((char *)$1, " "), 
                                          typeDef[cur].baseType);
         free(typeDef[cur].baseType);
         typeDef[cur].baseType = newBaseType;
      }
      else {
         typeDef[cur].baseType = (char *)$1;
      }
   }
   ;

vsidef_typebase_more: /* empty */
   | vsidef_typebase_more VSI_TOK_WORD
   {
      VSITab *typeDefTab = &vsiTypeDefTab;
      VSITypeDefTabEntry *typeDef = (VSITypeDefTabEntry *)typeDefTab->entries;
      int cur = typeDefTab->nEnts;

      // append
      if (typeDef[cur].baseType) {
         char *newBaseType = VSIConcatStr(VSIConcatStr(typeDef[cur].baseType, " "), 
                                          (char *)$2);
         free(typeDef[cur].baseType);
         typeDef[cur].baseType = newBaseType;
      }
      else {
         typeDef[cur].baseType = (char *)$2;
      }
   }
   ;

vsidef_struct:
   VSI_TOK_STRUCT '(' VSI_TOK_WORD ',' vsidef_quotedstr ')' '{' vsidef_struct_fields '}' ';'
   {
      VSITab *structFieldTab = &vsiStructFieldTab;
      VSIStructFieldTabEntry *structField = 
                           (VSIStructFieldTabEntry *) structFieldTab->entries;
      VSITab *typeDefTab = &vsiTypeDefTab;
      VSITypeDefTabEntry *typeDef = (VSITypeDefTabEntry *)typeDefTab->entries;
      int cur = typeDefTab->nEnts;

      typeDef[cur].type = VSITypeStruct;
      typeDef[cur].typeName = strdup((char *)$3);
      typeDef[cur].helpStr = strdup((char *)$5);
      typeDef[cur].baseType = NULL;

      typeDef[cur].u.structType.nFields = curStructFieldsCount;
      typeDef[cur].u.structType.fieldEnts = 
                                        &structField[curStructFieldsStartIdx];

      typeDefTab->nEnts++;
      if ( typeDefTab->nEnts >= typeDefTab->tabSize ) {
         VSIReAllocTable(typeDefTab);
      }

      /*
       * Reset fields count and starting index for next struct
       */
      curStructFieldsCount = 0;
      curStructFieldsStartIdx = structFieldTab->nEnts;
   }
   ;

vsidef_struct_fields: /* empty */
   | vsidef_struct_field vsidef_struct_fields
   ;

vsidef_struct_field:
   VSI_TOK_STRUCT_FIELD '(' VSI_TOK_WORD ',' VSI_TOK_WORD ',' vsidef_quotedstr ')' ';'
   {
      int baseTypeIdx, cur;
      VSITab *structFieldTab = &vsiStructFieldTab;
      VSIStructFieldTabEntry *structField = 
                           (VSIStructFieldTabEntry *) structFieldTab->entries;
      VSITab *typeDefTab = &vsiTypeDefTab;
      VSITypeDefTabEntry *typeDef = (VSITypeDefTabEntry *)typeDefTab->entries;

      /*
       * Validate type
       */
      baseTypeIdx = VSISearchTypeDef(typeDefTab, (char *)$3);
      if (baseTypeIdx == -1) {
         yyerror("unknown type in struct field");
         exit(1);
      }

      cur = structFieldTab->nEnts;
      structField[cur].fieldName = strdup((char *)$5);
      structField[cur].fieldTypeDefIdx = baseTypeIdx;
      structField[cur].helpStr = strdup((char *)$7);

      structFieldTab->nEnts++;
      if (structFieldTab->nEnts >= structFieldTab->tabSize) {
         VSIReAllocTable(structFieldTab);
      }

      curStructFieldsCount++;
   }
   ;

vsidef_quotedstr:
   VSI_TOK_QUOTEDSTR vsidef_quotedstr_more
   {
      if (curQuotedStr == NULL) {
         curQuotedStr = strdup((char *)$1);
      }
      else {
         char *newQuotedStr = VSIConcatStr((char *)$1, curQuotedStr);
         free(curQuotedStr);
         curQuotedStr = newQuotedStr;
      }

      $$ = (int)curQuotedStr;
      curQuotedStr = NULL;
   }
   ;

vsidef_quotedstr_more:  /* empty */
   | vsidef_quotedstr_more VSI_TOK_QUOTEDSTR
   {
      if (curQuotedStr == NULL) {
         curQuotedStr = strdup((char *)$2);
      }
      else {
         char *newQuotedStr = VSIConcatStr(curQuotedStr, (char *)$2);
         free(curQuotedStr);
         curQuotedStr = newQuotedStr;
      }
   }
   ;

vsidef_integer_ignore:
   VSI_TOK_INTEGER
   ;

vsidef_word_ignore:
   VSI_TOK_WORD
   ;

vsidef_quotedstr_ignore: 
   VSI_TOK_QUOTEDSTR
   ;

vsidef_openparen_ignore:
   '('
   ;

vsidef_closeparen_ignore:
   ')'
   ;

vsidef_comma_ignore:
   ','
   ;

vsidef_openbrace_ignore:
   '{'
   ;

vsidef_closebrace_ignore:
   '}'
   ;

vsidef_terminal_ignore:
   ';'
   ;

%%


