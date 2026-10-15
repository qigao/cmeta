#include "salts_unicode.h"
#include "tinytest.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SALTS_UNICODE_CASEFOLD_TEST_DATA
#error SALTS_UNICODE_CASEFOLD_TEST_DATA must name the pinned Unicode 17 file
#endif

static size_t encode_scalar(uint32_t scalar, char result[4]) {
  if (scalar <= 0x7fu) { result[0]=(char)scalar; return 1u; }
  if (scalar <= 0x7ffu) {
    result[0]=(char)(0xc0u | (scalar>>6u));
    result[1]=(char)(0x80u | (scalar&0x3fu)); return 2u;
  }
  if (scalar <= 0xffffu) {
    result[0]=(char)(0xe0u | (scalar>>12u));
    result[1]=(char)(0x80u | ((scalar>>6u)&0x3fu));
    result[2]=(char)(0x80u | (scalar&0x3fu)); return 3u;
  }
  result[0]=(char)(0xf0u | (scalar>>18u));
  result[1]=(char)(0x80u | ((scalar>>12u)&0x3fu));
  result[2]=(char)(0x80u | ((scalar>>6u)&0x3fu));
  result[3]=(char)(0x80u | (scalar&0x3fu)); return 4u;
}

static int test_one_mapping(const char *line) {
  char *end = NULL;
  const unsigned long cp = strtoul(line, &end, 16);
  const char *p;
  uint32_t mapped[3] = {0u,0u,0u};
  size_t mapped_count = 0u;
  char source[4], expected[12], actual[13];
  size_t source_bytes, expected_bytes=0u, written=SIZE_MAX, measured=SIZE_MAX;
  if(end==line || *end!=';' || cp>0x10fffful)return -1;
  p=end+1u;
  while(*p==' ')++p;
  if((*p!='C' && *p!='F') || p[1]!=';')return 0;
  p+=2;
  while(*p==' ')++p;
  while(*p && *p!=';') {
    unsigned long v = strtoul(p,&end,16);
    if(end==p || v>0x10fffful || mapped_count>=3u)return -1;
    mapped[mapped_count++]=(uint32_t)v;
    p=end;
    while(*p==' ')++p;
  }
  if(!mapped_count || *p!=';')return -1;
  for(size_t i=0u;i<mapped_count;++i)
    expected_bytes += encode_scalar(mapped[i],expected+expected_bytes);
  source_bytes=encode_scalar((uint32_t)cp,source);

  if(salts_unicode_case_fold(vstr_from_buf(source,source_bytes),
                             NULL,0u,&measured)!=SALTS_UNICODE_OK ||
     measured!=expected_bytes)return -1;
  memset(actual,0x5a,sizeof(actual));
  if(salts_unicode_case_fold(vstr_from_buf(source,source_bytes),
                             actual,expected_bytes,&written)!=SALTS_UNICODE_OK ||
     written!=expected_bytes || memcmp(actual,expected,expected_bytes)!=0 ||
     (unsigned char)actual[expected_bytes]!=0x5au)return -1;
  return 1;
}

spec("Salts Unicode 17 default full C/F case folding") {
  it("applies context-free expansions and preserves buffers on errors") {
    static const struct { const char *source; const char *expected; } cases[]={
      {"\xc3\x9f","ss"},                    /* ß */
      {"\xe1\xba\x9e","ss"},                /* ẞ */
      {"\xcf\x82","\xcf\x83"},             /* final sigma to sigma */
      {"\xce\xa3\xce\xa3","\xcf\x83\xcf\x83"}, /* two capitals, no final context */
      {"\xc4\xb0","i\xcc\x87"},            /* dotted capital I, not Turkic T */
      {"\xe2\x84\xaa","k"},                 /* Kelvin sign */
      {"\xef\xac\x83","ffi"},               /* ligature expansion */
      {"Stra\xc3\x9f""e","strasse"}
    };
    for(size_t i=0u;i<sizeof(cases)/sizeof(cases[0]);++i) {
      char result[64];size_t n=SIZE_MAX,required=SIZE_MAX;
      memset(result,0x35,sizeof(result));
      check_equal(salts_unicode_case_fold(vstr_from_cstr(cases[i].source),
                                          NULL,0u,&required),SALTS_UNICODE_OK);
      check_equal(required,strlen(cases[i].expected));
      check_equal(salts_unicode_case_fold(vstr_from_cstr(cases[i].source),
                                          result,sizeof(result),&n),SALTS_UNICODE_OK);
      check_equal(n,required);
      check_equal(memcmp(result,cases[i].expected,n),0);
      check_equal((unsigned char)result[n],0x35u);
    }

    const char embedded[3]={'A','\0','B'};
    char out[8];size_t count=0xbadfu;
    memset(out,0x5a,sizeof(out));
    check_equal(salts_unicode_case_fold(vstr_from_buf(embedded,sizeof embedded),
                                         out,sizeof out,&count),SALTS_UNICODE_OK);
    check_equal(count,3u);
    check_equal(memcmp(out,"a\0b",3u),0);

    count=0xbadfu;
    memset(out,0x5a,sizeof(out));
    check_equal(salts_unicode_case_fold(vstr_from_cstr("\xe1\xba\x9e"),
                                         out,1u,&count),SALTS_UNICODE_ERR_CAPACITY);
    check_equal(count,(size_t)0xbadfu);
    for(size_t i=0u;i<sizeof(out);++i)check_equal((unsigned char)out[i],0x5au);

    const unsigned char invalid[]={0xc0u,0xafu};
    check_equal(salts_unicode_case_fold(vstr_from_buf((const char *)invalid,sizeof invalid),
                                         out,sizeof out,&count),SALTS_UNICODE_ERR_INVALID_UTF8);
    check_equal(count,(size_t)0xbadfu);
    for(size_t i=0u;i<sizeof(out);++i)check_equal((unsigned char)out[i],0x5au);

    char overlapping[]="Input";
    check_equal(salts_unicode_case_fold(vstr_from_cstr(overlapping),
                                         overlapping,sizeof(overlapping),&count),
                SALTS_UNICODE_ERR_INVALID_ARGUMENT);
    check_equal(salts_unicode_case_fold(vstr_from_cstr("x"),NULL,1u,&count),
                SALTS_UNICODE_ERR_INVALID_ARGUMENT);
    check_equal(salts_unicode_case_fold(vstr_from_cstr("x"),out,sizeof out,NULL),
                SALTS_UNICODE_ERR_INVALID_ARGUMENT);
    check_equal(salts_unicode_case_fold(vstr_from_buf(NULL,0u),NULL,0u,&count),
                SALTS_UNICODE_OK);
    check_equal(count,0u);
  }

  it("matches every authoritative Unicode 17 CaseFolding.txt C/F mapping") {
    FILE *fp=fopen(SALTS_UNICODE_CASEFOLD_TEST_DATA,"rb");
    char line[2048];
    size_t mapped=0u;
    int passed=fp!=NULL;
    check_not_null(fp);
    if(fp==NULL)return;
    while(fgets(line,sizeof line,fp)!=NULL) {
      char *payload=line;
      int result;
      while(*payload==' ' || *payload=='\t')++payload;
      if(*payload=='#' || *payload=='\n' || *payload=='\r' || *payload=='\0')
        continue;
      if(strchr(line,'\n')==NULL && !feof(fp)) {passed=0;break;}
      result=test_one_mapping(payload);
      if(result<0) {
        fprintf(stderr,"Unicode 17 CaseFolding mismatch: %s",line);
        passed=0;break;
      }
      mapped+=(size_t)result;
    }
    if(ferror(fp))passed=0;
    fclose(fp);
    check_true(passed);
    check_equal(mapped,(size_t)1585);
  }
}
