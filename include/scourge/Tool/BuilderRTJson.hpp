// ============================================================================
//  Scourge 编译器 — AOT C 运行时 · JSON 片段（Builder 分块专用）
// ----------------------------------------------------------------------------
//  职责边界：kRuntimeC / kStrRuntimeC / kStdRuntimeC 之后的追加片段，build
//  时拼接写入 <out>_rt.c——依赖 kCollRuntimeC 的 SgList 与 kRuntimeC 的
//  sg_dup / sg_panic 定义，缓存键
//  rtObjKey 已计入本片段文本。仅供 Builder.cpp 引用（内部共享头）。
//  token 化解析器 vendored 自 jsmn（MIT License, Copyright (c) 2010
//  Serge Zaitsev；单头、非递归 tokenizer，原文内嵌保留版权声明），
//  上层建树/转义/编码为适配层。
//  语义契约：与 VM jsonBuiltin/jsonMethod（VMJson.cpp）panic 消息与
//  输出文本逐字一致，双模差分由 verify_json.ps1 探针锁定。
// ============================================================================
#pragma once

namespace scourge {

inline const char kJsonRuntimeC[] = R"RT(
/* ==== JSON：动态 Json 值（0.9.4 M 专项；与 VM jsonBuiltin/jsonMethod 一致） ==== */
/*
 * MIT License
 *
 * Copyright (c) 2010 Serge Zaitsev
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#define JSMN_STATIC
#define JSMN_IMPLEMENTATION
#ifndef JSMN_H
#define JSMN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef JSMN_STATIC
#define JSMN_API static
#else
#define JSMN_API extern
#endif

/**
 * JSON type identifier. Basic types are:
 * 	o Object
 * 	o Array
 * 	o String
 * 	o Other primitive: number, boolean (true/false) or null
 */
typedef enum {
  JSMN_UNDEFINED = 0,
  JSMN_OBJECT = 1 << 0,
  JSMN_ARRAY = 1 << 1,
  JSMN_STRING = 1 << 2,
  JSMN_PRIMITIVE = 1 << 3
} jsmntype_t;

enum jsmnerr {
  /* Not enough tokens were provided */
  JSMN_ERROR_NOMEM = -1,
  /* Invalid character inside JSON string */
  JSMN_ERROR_INVAL = -2,
  /* The string is not a full JSON packet, more bytes expected */
  JSMN_ERROR_PART = -3
};

/**
 * JSON token description.
 * type		type (object, array, string etc.)
 * start	start position in JSON data string
 * end		end position in JSON data string
 */
typedef struct jsmntok {
  jsmntype_t type;
  int start;
  int end;
  int size;
#ifdef JSMN_PARENT_LINKS
  int parent;
#endif
} jsmntok_t;

/**
 * JSON parser. Contains an array of token blocks available. Also stores
 * the string being parsed now and current position in that string.
 */
typedef struct jsmn_parser {
  unsigned int pos;     /* offset in the JSON string */
  unsigned int toknext; /* next token to allocate */
  int toksuper;         /* superior token node, e.g. parent object or array */
} jsmn_parser;

/**
 * Create JSON parser over an array of tokens
 */
JSMN_API void jsmn_init(jsmn_parser *parser);

/**
 * Run JSON parser. It parses a JSON data string into and array of tokens, each
 * describing
 * a single JSON object.
 */
JSMN_API int jsmn_parse(jsmn_parser *parser, const char *js, const size_t len,
                        jsmntok_t *tokens, const unsigned int num_tokens);

#ifndef JSMN_HEADER
/**
 * Allocates a fresh unused token from the token pool.
 */
static jsmntok_t *jsmn_alloc_token(jsmn_parser *parser, jsmntok_t *tokens,
                                   const size_t num_tokens) {
  jsmntok_t *tok;
  if (parser->toknext >= num_tokens) {
    return NULL;
  }
  tok = &tokens[parser->toknext++];
  tok->start = tok->end = -1;
  tok->size = 0;
#ifdef JSMN_PARENT_LINKS
  tok->parent = -1;
#endif
  return tok;
}

/**
 * Fills token type and boundaries.
 */
static void jsmn_fill_token(jsmntok_t *token, const jsmntype_t type,
                            const int start, const int end) {
  token->type = type;
  token->start = start;
  token->end = end;
  token->size = 0;
}

/**
 * Fills next available token with JSON primitive.
 */
static int jsmn_parse_primitive(jsmn_parser *parser, const char *js,
                                const size_t len, jsmntok_t *tokens,
                                const size_t num_tokens) {
  jsmntok_t *token;
  int start;

  start = parser->pos;

  for (; parser->pos < len && js[parser->pos] != '\0'; parser->pos++) {
    switch (js[parser->pos]) {
#ifndef JSMN_STRICT
    /* In strict mode primitive must be followed by "," or "}" or "]" */
    case ':':
#endif
    case '\t':
    case '\r':
    case '\n':
    case ' ':
    case ',':
    case ']':
    case '}':
      goto found;
    default:
                   /* to quiet a warning from gcc*/
      break;
    }
    if (js[parser->pos] < 32 || js[parser->pos] >= 127) {
      parser->pos = start;
      return JSMN_ERROR_INVAL;
    }
  }
#ifdef JSMN_STRICT
  /* In strict mode primitive must be followed by a comma/object/array */
  parser->pos = start;
  return JSMN_ERROR_PART;
#endif

found:
  if (tokens == NULL) {
    parser->pos--;
    return 0;
  }
  token = jsmn_alloc_token(parser, tokens, num_tokens);
  if (token == NULL) {
    parser->pos = start;
    return JSMN_ERROR_NOMEM;
  }
  jsmn_fill_token(token, JSMN_PRIMITIVE, start, parser->pos);
#ifdef JSMN_PARENT_LINKS
  token->parent = parser->toksuper;
#endif
  parser->pos--;
  return 0;
}

/**
 * Fills next token with JSON string.
 */
static int jsmn_parse_string(jsmn_parser *parser, const char *js,
                             const size_t len, jsmntok_t *tokens,
                             const size_t num_tokens) {
  jsmntok_t *token;

  int start = parser->pos;
  
  /* Skip starting quote */
  parser->pos++;
  
  for (; parser->pos < len && js[parser->pos] != '\0'; parser->pos++) {
    char c = js[parser->pos];

    /* Quote: end of string */
    if (c == '\"') {
      if (tokens == NULL) {
        return 0;
      }
      token = jsmn_alloc_token(parser, tokens, num_tokens);
      if (token == NULL) {
        parser->pos = start;
        return JSMN_ERROR_NOMEM;
      }
      jsmn_fill_token(token, JSMN_STRING, start + 1, parser->pos);
#ifdef JSMN_PARENT_LINKS
      token->parent = parser->toksuper;
#endif
      return 0;
    }

    /* Backslash: Quoted symbol expected */
    if (c == '\\' && parser->pos + 1 < len) {
      int i;
      parser->pos++;
      switch (js[parser->pos]) {
      /* Allowed escaped symbols */
      case '\"':
      case '/':
      case '\\':
      case 'b':
      case 'f':
      case 'r':
      case 'n':
      case 't':
        break;
      /* Allows escaped symbol \uXXXX */
      case 'u':
        parser->pos++;
        for (i = 0; i < 4 && parser->pos < len && js[parser->pos] != '\0';
             i++) {
          /* If it isn't a hex character we have an error */
          if (!((js[parser->pos] >= 48 && js[parser->pos] <= 57) ||   /* 0-9 */
                (js[parser->pos] >= 65 && js[parser->pos] <= 70) ||   /* A-F */
                (js[parser->pos] >= 97 && js[parser->pos] <= 102))) { /* a-f */
            parser->pos = start;
            return JSMN_ERROR_INVAL;
          }
          parser->pos++;
        }
        parser->pos--;
        break;
      /* Unexpected symbol */
      default:
        parser->pos = start;
        return JSMN_ERROR_INVAL;
      }
    }
  }
  parser->pos = start;
  return JSMN_ERROR_PART;
}

/**
 * Parse JSON string and fill tokens.
 */
JSMN_API int jsmn_parse(jsmn_parser *parser, const char *js, const size_t len,
                        jsmntok_t *tokens, const unsigned int num_tokens) {
  int r;
  int i;
  jsmntok_t *token;
  int count = parser->toknext;

  for (; parser->pos < len && js[parser->pos] != '\0'; parser->pos++) {
    char c;
    jsmntype_t type;

    c = js[parser->pos];
    switch (c) {
    case '{':
    case '[':
      count++;
      if (tokens == NULL) {
        break;
      }
      token = jsmn_alloc_token(parser, tokens, num_tokens);
      if (token == NULL) {
        return JSMN_ERROR_NOMEM;
      }
      if (parser->toksuper != -1) {
        jsmntok_t *t = &tokens[parser->toksuper];
#ifdef JSMN_STRICT
        /* In strict mode an object or array can't become a key */
        if (t->type == JSMN_OBJECT) {
          return JSMN_ERROR_INVAL;
        }
#endif
        t->size++;
#ifdef JSMN_PARENT_LINKS
        token->parent = parser->toksuper;
#endif
      }
      token->type = (c == '{' ? JSMN_OBJECT : JSMN_ARRAY);
      token->start = parser->pos;
      parser->toksuper = parser->toknext - 1;
      break;
    case '}':
    case ']':
      if (tokens == NULL) {
        break;
      }
      type = (c == '}' ? JSMN_OBJECT : JSMN_ARRAY);
#ifdef JSMN_PARENT_LINKS
      if (parser->toknext < 1) {
        return JSMN_ERROR_INVAL;
      }
      token = &tokens[parser->toknext - 1];
      for (;;) {
        if (token->start != -1 && token->end == -1) {
          if (token->type != type) {
            return JSMN_ERROR_INVAL;
          }
          token->end = parser->pos + 1;
          parser->toksuper = token->parent;
          break;
        }
        if (token->parent == -1) {
          if (token->type != type || parser->toksuper == -1) {
            return JSMN_ERROR_INVAL;
          }
          break;
        }
        token = &tokens[token->parent];
      }
#else
      for (i = parser->toknext - 1; i >= 0; i--) {
        token = &tokens[i];
        if (token->start != -1 && token->end == -1) {
          if (token->type != type) {
            return JSMN_ERROR_INVAL;
          }
          parser->toksuper = -1;
          token->end = parser->pos + 1;
          break;
        }
      }
      /* Error if unmatched closing bracket */
      if (i == -1) {
        return JSMN_ERROR_INVAL;
      }
      for (; i >= 0; i--) {
        token = &tokens[i];
        if (token->start != -1 && token->end == -1) {
          parser->toksuper = i;
          break;
        }
      }
#endif
      break;
    case '\"':
      r = jsmn_parse_string(parser, js, len, tokens, num_tokens);
      if (r < 0) {
        return r;
      }
      count++;
      if (parser->toksuper != -1 && tokens != NULL) {
        tokens[parser->toksuper].size++;
      }
      break;
    case '\t':
    case '\r':
    case '\n':
    case ' ':
      break;
    case ':':
      parser->toksuper = parser->toknext - 1;
      break;
    case ',':
      if (tokens != NULL && parser->toksuper != -1 &&
          tokens[parser->toksuper].type != JSMN_ARRAY &&
          tokens[parser->toksuper].type != JSMN_OBJECT) {
#ifdef JSMN_PARENT_LINKS
        parser->toksuper = tokens[parser->toksuper].parent;
#else
        for (i = parser->toknext - 1; i >= 0; i--) {
          if (tokens[i].type == JSMN_ARRAY || tokens[i].type == JSMN_OBJECT) {
            if (tokens[i].start != -1 && tokens[i].end == -1) {
              parser->toksuper = i;
              break;
            }
          }
        }
#endif
      }
      break;
#ifdef JSMN_STRICT
    /* In strict mode primitives are: numbers and booleans */
    case '-':
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
    case 't':
    case 'f':
    case 'n':
      /* And they must not be keys of the object */
      if (tokens != NULL && parser->toksuper != -1) {
        const jsmntok_t *t = &tokens[parser->toksuper];
        if (t->type == JSMN_OBJECT ||
            (t->type == JSMN_STRING && t->size != 0)) {
          return JSMN_ERROR_INVAL;
        }
      }
#else
    /* In non-strict mode every unquoted value is a primitive */
    default:
#endif
      r = jsmn_parse_primitive(parser, js, len, tokens, num_tokens);
      if (r < 0) {
        return r;
      }
      count++;
      if (parser->toksuper != -1 && tokens != NULL) {
        tokens[parser->toksuper].size++;
      }
      break;

#ifdef JSMN_STRICT
    /* Unexpected char in strict mode */
    default:
      return JSMN_ERROR_INVAL;
#endif
    }
  }

  if (tokens != NULL) {
    for (i = parser->toknext - 1; i >= 0; i--) {
      /* Unmatched opened object or array */
      if (tokens[i].start != -1 && tokens[i].end == -1) {
        return JSMN_ERROR_PART;
      }
    }
  }

  return count;
}

/**
 * Creates a new parser based over a given buffer with an array of tokens
 * available.
 */
JSMN_API void jsmn_init(jsmn_parser *parser) {
  parser->pos = 0;
  parser->toknext = 0;
  parser->toksuper = -1;
}

#endif /* JSMN_HEADER */

#ifdef __cplusplus
}
#endif

#endif /* JSMN_H */


/* ---- 适配层：jsmn token 树 → SgJson 动态值（与 VM VMJson.cpp 同规范） ----
 * 规范要点（双模逐字一致）：
 *   * 数字：文本含 '.'/'e'/'E' → double；否则 strtoll（ERANGE → 解析失败）；
 *     strtod 上溢(±inf) → 解析失败；下溢 0 接受。
 *   * 字符串：\" \\ \/ \b \f \n \r \t + \uXXXX（代理对配对；孤立代理失败）；
 *     裸控制字符失败；UTF-8 字节透传。
 *   * 嵌套深度上限 128；对象重复键后者覆盖值、保留首插位置（线性表同位覆盖）。
 *   * dump：紧凑编码；对象插入序；float %.17g；控制字符 \u00xx 小写 hex。
 *   * at(i) 越界 panic；get(k) 缺键返回 null 单例（不 panic）。
 * 内存：SgJson 树按程序生命周期持有（与 sg_str_split 同泄漏式设计）。
 */

#include <errno.h>

typedef struct SgJson SgJson;

typedef struct {
    const char* k;
    SgJson* v;
} SgJsonEnt;


struct SgJson {
    int32_t tag; /* 0=null 1=bool 2=int 3=float 4=str 5=arr 6=obj */
    int64_t i;   /* Bool(0/1) / Int */
    double f;    /* Float */
    char* s;     /* Str（UTF-8 字节，转义已解码） */
    /* 数组形态 */
    SgJson** items;
    int64_t n, cap;
    /* 对象形态：插入序线性表（成员通常少，线性扫描缓存友好） */
    SgJsonEnt* ents;
    int64_t on, ocap;
};

static const char* const kSgJsonKindName[7] = {
    "null", "bool", "int", "float", "str", "array", "object"};

static SgJson* sg_json_new(int32_t tag) {
    SgJson* j = (SgJson*)malloc(sizeof(SgJson));
    if (!j) exit(1);
    j->tag = tag;
    j->i = 0;
    j->f = 0.0;
    j->s = NULL;
    j->items = NULL;
    j->n = 0;
    j->cap = 0;
    j->ents = NULL;
    j->on = 0;
    j->ocap = 0;
    return j;
}

static SgJson sg_json_null_singleton = {0, 0, 0.0, NULL, NULL, 0, 0, NULL, 0, 0};

static const char* sg_json_kind_name(int32_t tag) {
    return (tag >= 0 && tag < 7) ? kSgJsonKindName[tag] : "?";
}

static void sg_json_arr_push(SgJson* j, SgJson* e) {
    if (j->n == j->cap) {
        j->cap = j->cap ? j->cap * 2 : 4;
        j->items = (SgJson**)realloc(j->items, sizeof(SgJson*) * (size_t)j->cap);
        if (!j->items) exit(1);
    }
    j->items[j->n++] = e;
}

static void sg_json_obj_put(SgJson* j, const char* k, size_t kn, SgJson* v) {
    for (int64_t e = 0; e < j->on; ++e) {
        if (strlen(j->ents[e].k) == kn && memcmp(j->ents[e].k, k, kn) == 0) {
            j->ents[e].v = v; /* 重复键：后者覆盖值、保留首插位置 */
            return;
        }
    }
    if (j->on == j->ocap) {
        j->ocap = j->ocap ? j->ocap * 2 : 4;
        j->ents = (SgJsonEnt*)realloc(j->ents,
                                      sizeof(SgJsonEnt) * (size_t)j->ocap);
        if (!j->ents) exit(1);
    }
    char* kd = (char*)malloc(kn + 1);
    if (!kd) exit(1);
    memcpy(kd, k, kn);
    kd[kn] = 0;
    j->ents[j->on].k = kd;
    j->ents[j->on].v = v;
    ++j->on;
}

/* int 码点 → UTF-8（1-4 字节） */
static int sg_json_utf8_put(int cp, char* out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static int sg_json_hex4(const char* js, int pos) {
    int v = 0;
    for (int k = 0; k < 4; ++k) {
        char c = js[pos + k];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    return v;
}

/* 解析 JSON 字符串 token（[start,end) 为不含引号的内容区间——jsmn 语义；
 * 含原文转义）→ malloc 的 UTF-8 字节串；失败返回 NULL。 */
static char* sg_json_unescape(const char* js, int start, int end) {
    char* out = (char*)malloc((size_t)(end - start) + 1);
    if (!out) exit(1);
    size_t w = 0;
    for (int p = start; p < end;) {
        unsigned char c = (unsigned char)js[p];
        if (c != '\\') {
            if (c < 0x20) { free(out); return NULL; } /* 裸控制字符 */
            out[w++] = (char)c;
            ++p;
            continue;
        }
        if (p + 1 >= end) { free(out); return NULL; }
        char e = js[p + 1];
        p += 2;
        switch (e) {
        case '"': out[w++] = '"'; break;
        case '\\': out[w++] = '\\'; break;
        case '/': out[w++] = '/'; break;
        case 'b': out[w++] = '\b'; break;
        case 'f': out[w++] = '\f'; break;
        case 'n': out[w++] = '\n'; break;
        case 'r': out[w++] = '\r'; break;
        case 't': out[w++] = '\t'; break;
        case 'u': {
            if (p + 4 > end) { free(out); return NULL; }
            int cp = sg_json_hex4(js, p);
            if (cp < 0) { free(out); return NULL; }
            p += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                /* 高代理：必须紧跟 \uDC00-\uDFFF */
                if (p + 6 > end || js[p] != '\\' || js[p + 1] != 'u') {
                    free(out);
                    return NULL;
                }
                int lo = sg_json_hex4(js, p + 2);
                if (lo < 0xDC00 || lo > 0xDFFF) { free(out); return NULL; }
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                p += 6;
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                free(out);
                return NULL; /* 孤立低代理 */
            }
            w += (size_t)sg_json_utf8_put(cp, out + w);
            break;
        }
        default: free(out); return NULL; /* 非法转义 */
        }
    }
    out[w] = 0;
    return out;
}

static SgJson* sg_json_build(const jsmntok_t* toks, int ntok, int* pos,
                             const char* js, int depth, const char* fn,
                             const char* file, int64_t line) {
    if (*pos >= ntok) sg_panic(fn, file, line, "json_decode() 输入不是有效 JSON");
    const jsmntok_t* t = &toks[(*pos)++];
    if (t->type == JSMN_STRING) {
        SgJson* j = sg_json_new(4);
        j->s = sg_json_unescape(js, t->start, t->end);
        if (!j->s) sg_panic(fn, file, line, "json_decode() 输入不是有效 JSON");
        return j;
    }
    if (t->type == JSMN_PRIMITIVE) {
        int len = t->end - t->start;
        const char* p = js + t->start;
        if (len == 4 && memcmp(p, "true", 4) == 0) {
            SgJson* j = sg_json_new(1);
            j->i = 1;
            return j;
        }
        if (len == 5 && memcmp(p, "false", 5) == 0) return sg_json_new(1);
        if (len == 4 && memcmp(p, "null", 4) == 0) return sg_json_new(0);
        /* 数字：int / float 判定按文本（与 VM 同规范） */
        char* text = (char*)malloc((size_t)len + 1);
        if (!text) exit(1);
        memcpy(text, p, (size_t)len);
        text[len] = 0;
        int isFloat = 0;
        for (int k = 0; k < len; ++k) {
            if (text[k] == '.' || text[k] == 'e' || text[k] == 'E') {
                isFloat = 1;
                break;
            }
        }
        SgJson* j;
        if (!isFloat) {
            errno = 0;
            long long iv = strtoll(text, NULL, 10);
            if (errno == ERANGE) {
                free(text);
                sg_panic(fn, file, line, "json_decode() 输入不是有效 JSON");
            }
            j = sg_json_new(2);
            j->i = (int64_t)iv;
        } else {
            errno = 0;
            double fv = strtod(text, NULL);
            if (errno == ERANGE && isinf(fv)) {
                free(text);
                sg_panic(fn, file, line, "json_decode() 输入不是有效 JSON");
            }
            j = sg_json_new(3);
            j->f = fv;
        }
        free(text);
        return j;
    }
    if (t->type == JSMN_ARRAY) {
        if (++depth > 128)
            sg_panic(fn, file, line, "json_decode() 嵌套深度超上限 128");
        SgJson* j = sg_json_new(5);
        for (int k = 0; k < t->size; ++k)
            sg_json_arr_push(j, sg_json_build(toks, ntok, pos, js, depth, fn,
                                              file, line));
        return j;
    }
    if (t->type == JSMN_OBJECT) {
        if (++depth > 128)
            sg_panic(fn, file, line, "json_decode() 嵌套深度超上限 128");
        SgJson* j = sg_json_new(6);
        for (int k = 0; k < t->size; ++k) {
            const jsmntok_t* kt = &toks[*pos];
            if (kt->type != JSMN_STRING)
                sg_panic(fn, file, line, "json_decode() 输入不是有效 JSON");
            (*pos)++;
            SgJson* key = sg_json_new(4);
            key->s = sg_json_unescape(js, kt->start, kt->end);
            if (!key->s)
                sg_panic(fn, file, line, "json_decode() 输入不是有效 JSON");
            SgJson* v = sg_json_build(toks, ntok, pos, js, depth, fn, file, line);
            sg_json_obj_put(j, key->s, strlen(key->s), v);
            free(key->s);
            free(key);
        }
        return j;
    }
    sg_panic(fn, file, line, "json_decode() 输入不是有效 JSON");
}

SgJson* sg_json_decode(const char* s, const char* fn, const char* file,
                       int64_t line) {
    size_t len = strlen(s);
    jsmn_parser parser;
    int cap = 256;
    for (;;) {
        jsmntok_t* toks = (jsmntok_t*)malloc(sizeof(jsmntok_t) * (size_t)cap);
        if (!toks) exit(1);
        jsmn_init(&parser);
        int ntok = jsmn_parse(&parser, s, len, toks, (size_t)cap);
        if (ntok == JSMN_ERROR_NOMEM) {
            free(toks);
            cap *= 2;
            if (cap > (1 << 24)) /* 16M token 上限，防病态输入无限翻倍 */
                sg_panic(fn, file, line, "json_decode() 输入不是有效 JSON");
            continue;
        }
        if (ntok < 0) {
            free(toks);
            sg_panic(fn, file, line, "json_decode() 输入不是有效 JSON");
        }
        int pos = 0;
        SgJson* root = sg_json_build(toks, ntok, &pos, s, 0, fn, file, line);
        free(toks);
        return root;
    }
}

int sg_json_is(SgJson* j, int32_t tag) { return j->tag == tag; }

int64_t sg_json_as_int(SgJson* j, const char* fn, const char* file,
                       int64_t line) {
    if (j->tag != 2) {
        char pmsg[80];
        sprintf(pmsg, "as_int() 仅支持整数形态（实际为 %s）",
                sg_json_kind_name(j->tag));
        sg_panic(fn, file, line, pmsg);
    }
    return j->i;
}

double sg_json_as_float(SgJson* j, const char* fn, const char* file,
                        int64_t line) {
    if (j->tag == 3) return j->f;
    if (j->tag == 2) return (double)j->i;
    char pmsg[80];
    sprintf(pmsg, "as_float() 仅支持数值形态（实际为 %s）",
            sg_json_kind_name(j->tag));
    sg_panic(fn, file, line, pmsg);
}

int sg_json_as_bool(SgJson* j, const char* fn, const char* file,
                    int64_t line) {
    if (j->tag != 1) {
        char pmsg[80];
        sprintf(pmsg, "as_bool() 仅支持布尔形态（实际为 %s）",
                sg_json_kind_name(j->tag));
        sg_panic(fn, file, line, pmsg);
    }
    return (int)j->i;
}

const char* sg_json_as_str(SgJson* j, const char* fn, const char* file,
                           int64_t line) {
    if (j->tag != 4) {
        char pmsg[80];
        sprintf(pmsg, "as_str() 仅支持字符串形态（实际为 %s）",
                sg_json_kind_name(j->tag));
        sg_panic(fn, file, line, pmsg);
    }
    return j->s;
}

SgJson* sg_json_get_idx(SgJson* j, int64_t idx, const char* fn,
                        const char* file, int64_t line) {
    if (j->tag != 5) {
        char pmsg[80];
        sprintf(pmsg, "at() 仅支持数组形态（实际为 %s）",
                sg_json_kind_name(j->tag));
        sg_panic(fn, file, line, pmsg);
    }
    if (idx < 0 || idx >= j->n)
        sg_panic(fn, file, line, "at() 数组下标越界");
    return j->items[idx];
}

SgJson* sg_json_get_key(SgJson* j, const char* k, const char* fn,
                        const char* file, int64_t line) {
    if (j->tag != 6) {
        char pmsg[80];
        sprintf(pmsg, "get() 仅支持对象形态（实际为 %s）",
                sg_json_kind_name(j->tag));
        sg_panic(fn, file, line, pmsg);
    }
    size_t kn = strlen(k);
    for (int64_t e = 0; e < j->on; ++e)
        if (strlen(j->ents[e].k) == kn && memcmp(j->ents[e].k, k, kn) == 0)
            return j->ents[e].v;
    return &sg_json_null_singleton; /* 缺键返回 null（不 panic） */
}

int64_t sg_json_len(SgJson* j, const char* fn, const char* file,
                    int64_t line) {
    if (j->tag == 5) return j->n;
    if (j->tag == 6) return j->on;
    char pmsg[96];
    sprintf(pmsg, "len() 仅支持数组或对象形态（实际为 %s）",
            sg_json_kind_name(j->tag));
    sg_panic(fn, file, line, pmsg);
}

SgList* sg_json_keys(SgJson* j, const char* fn, const char* file,
                     int64_t line) {
    if (j->tag != 6) {
        char pmsg[80];
        sprintf(pmsg, "keys() 仅支持对象形态（实际为 %s）",
                sg_json_kind_name(j->tag));
        sg_panic(fn, file, line, pmsg);
    }
    SgList* out = sg_list_empty();
    out->len = j->on;
    out->etag = 2;
    out->data = malloc(sizeof(char*) * (size_t)(j->on > 0 ? j->on : 1));
    if (!out->data) exit(1);
    for (int64_t e = 0; e < j->on; ++e)
        ((char**)out->data)[e] = sg_dup(j->ents[e].k);
    return out;
}

/* ---- dump：紧凑 JSON 编码（与 VM jsonDump 逐字一致） ---- */

typedef struct {
    char* p;
    size_t n, cap;
} SgBuf;

static void sg_buf_put(SgBuf* b, const char* s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        b->cap = b->cap ? b->cap : 64;
        while (b->n + n + 1 > b->cap) b->cap *= 2;
        b->p = (char*)realloc(b->p, b->cap);
        if (!b->p) exit(1);
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void sg_buf_puts(SgBuf* b, const char* s) { sg_buf_put(b, s, strlen(s)); }

static void sg_json_dump_str(const char* s, SgBuf* b) {
    sg_buf_put(b, "\"", 1);
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
        switch (*p) {
        case '"': sg_buf_puts(b, "\\\""); break;
        case '\\': sg_buf_puts(b, "\\\\"); break;
        case '\b': sg_buf_puts(b, "\\b"); break;
        case '\f': sg_buf_puts(b, "\\f"); break;
        case '\n': sg_buf_puts(b, "\\n"); break;
        case '\r': sg_buf_puts(b, "\\r"); break;
        case '\t': sg_buf_puts(b, "\\t"); break;
        default:
            if (*p < 0x20) {
                char esc[8];
                sprintf(esc, "\\u%04x", *p);
                sg_buf_puts(b, esc);
            } else {
                sg_buf_put(b, (const char*)p, 1); /* UTF-8 字节透传 */
            }
        }
    }
    sg_buf_put(b, "\"", 1);
}

static void sg_json_dump_to(SgJson* j, SgBuf* b) {
    char num[48];
    switch (j->tag) {
    case 0: sg_buf_puts(b, "null"); break;
    case 1: sg_buf_puts(b, j->i ? "true" : "false"); break;
    case 2:
        sprintf(num, "%lld", (long long)j->i);
        sg_buf_puts(b, num);
        break;
    case 3:
        snprintf(num, sizeof(num), "%.17g", j->f);
        sg_buf_puts(b, num);
        break;
    case 4: sg_json_dump_str(j->s, b); break;
    case 5:
        sg_buf_put(b, "[", 1);
        for (int64_t k = 0; k < j->n; ++k) {
            if (k) sg_buf_put(b, ",", 1);
            sg_json_dump_to(j->items[k], b);
        }
        sg_buf_put(b, "]", 1);
        break;
    case 6:
        sg_buf_put(b, "{", 1);
        for (int64_t k = 0; k < j->on; ++k) {
            if (k) sg_buf_put(b, ",", 1);
            sg_json_dump_str(j->ents[k].k, b);
            sg_buf_put(b, ":", 1);
            sg_json_dump_to(j->ents[k].v, b);
        }
        sg_buf_put(b, "}", 1);
        break;
    }
}

const char* sg_json_dump(SgJson* j) {
    SgBuf b = {NULL, 0, 0};
    sg_json_dump_to(j, &b);
    if (!b.p) sg_buf_put(&b, "", 0);
    return b.p; /* 程序生命周期持有（与 str 家族同设计） */
}

void sg_print_json(SgJson* j) {
    SgBuf b = {NULL, 0, 0};
    sg_json_dump_to(j, &b);
    if (!b.p) sg_buf_put(&b, "", 0);
    fputs(b.p, stdout);
    free(b.p);
}
)RT";

} // namespace scourge
