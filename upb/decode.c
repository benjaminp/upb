
#include "upb/decode.h"

#include <setjmp.h>
#include <string.h>

#include "upb/upb.h"
#include "upb/upb.int.h"

/* Must be last. */
#include "upb/port_def.inc"

/* Maps descriptor type -> elem_size_lg2.  */
static const uint8_t desctype_to_elem_size_lg2[] = {
    -1,               /* invalid descriptor type */
    3,  /* DOUBLE */
    2,   /* FLOAT */
    3,   /* INT64 */
    3,  /* UINT64 */
    2,   /* INT32 */
    3,  /* FIXED64 */
    2,  /* FIXED32 */
    0,    /* BOOL */
    UPB_SIZE(3, 4),  /* STRING */
    UPB_SIZE(2, 3),  /* GROUP */
    UPB_SIZE(2, 3),  /* MESSAGE */
    UPB_SIZE(3, 4),  /* BYTES */
    2,  /* UINT32 */
    2,    /* ENUM */
    2,   /* SFIXED32 */
    3,   /* SFIXED64 */
    2,   /* SINT32 */
    3,   /* SINT64 */
};

/* Maps descriptor type -> upb map size.  */
static const uint8_t desctype_to_mapsize[] = {
    -1,                 /* invalid descriptor type */
    8,                  /* DOUBLE */
    4,                  /* FLOAT */
    8,                  /* INT64 */
    8,                  /* UINT64 */
    4,                  /* INT32 */
    8,                  /* FIXED64 */
    4,                  /* FIXED32 */
    1,                  /* BOOL */
    UPB_MAPTYPE_STRING, /* STRING */
    sizeof(void *),     /* GROUP */
    sizeof(void *),     /* MESSAGE */
    UPB_MAPTYPE_STRING, /* BYTES */
    4,                  /* UINT32 */
    4,                  /* ENUM */
    4,                  /* SFIXED32 */
    8,                  /* SFIXED64 */
    4,                  /* SINT32 */
    8,                  /* SINT64 */
};

static const unsigned fixed32_ok = (1 << UPB_DTYPE_FLOAT) |
                                   (1 << UPB_DTYPE_FIXED32) |
                                   (1 << UPB_DTYPE_SFIXED32);

static const unsigned fixed64_ok = (1 << UPB_DTYPE_DOUBLE) |
                                   (1 << UPB_DTYPE_FIXED64) |
                                   (1 << UPB_DTYPE_SFIXED64);

/* Op: an action to be performed for a wire-type/field-type combination. */
#define OP_SCALAR_LG2(n) (n)      /* n in [0, 2, 3] => op in [0, 2, 3] */
#define OP_STRING 4
#define OP_BYTES 5
#define OP_SUBMSG 6
/* Ops above are scalar-only. Repeated fields can use any op.  */
#define OP_FIXPCK_LG2(n) (n + 5)  /* n in [2, 3] => op in [7, 8] */
#define OP_VARPCK_LG2(n) (n + 9)  /* n in [0, 2, 3] => op in [9, 11, 12] */

static const int8_t varint_ops[19] = {
    -1,               /* field not found */
    -1,               /* DOUBLE */
    -1,               /* FLOAT */
    OP_SCALAR_LG2(3), /* INT64 */
    OP_SCALAR_LG2(3), /* UINT64 */
    OP_SCALAR_LG2(2), /* INT32 */
    -1,               /* FIXED64 */
    -1,               /* FIXED32 */
    OP_SCALAR_LG2(0), /* BOOL */
    -1,               /* STRING */
    -1,               /* GROUP */
    -1,               /* MESSAGE */
    -1,               /* BYTES */
    OP_SCALAR_LG2(2), /* UINT32 */
    OP_SCALAR_LG2(2), /* ENUM */
    -1,               /* SFIXED32 */
    -1,               /* SFIXED64 */
    OP_SCALAR_LG2(2), /* SINT32 */
    OP_SCALAR_LG2(3), /* SINT64 */
};

static const int8_t delim_ops[37] = {
    /* For non-repeated field type. */
    -1,        /* field not found */
    -1,        /* DOUBLE */
    -1,        /* FLOAT */
    -1,        /* INT64 */
    -1,        /* UINT64 */
    -1,        /* INT32 */
    -1,        /* FIXED64 */
    -1,        /* FIXED32 */
    -1,        /* BOOL */
    OP_STRING, /* STRING */
    -1,        /* GROUP */
    OP_SUBMSG, /* MESSAGE */
    OP_BYTES,  /* BYTES */
    -1,        /* UINT32 */
    -1,        /* ENUM */
    -1,        /* SFIXED32 */
    -1,        /* SFIXED64 */
    -1,        /* SINT32 */
    -1,        /* SINT64 */
    /* For repeated field type. */
    OP_FIXPCK_LG2(3), /* REPEATED DOUBLE */
    OP_FIXPCK_LG2(2), /* REPEATED FLOAT */
    OP_VARPCK_LG2(3), /* REPEATED INT64 */
    OP_VARPCK_LG2(3), /* REPEATED UINT64 */
    OP_VARPCK_LG2(2), /* REPEATED INT32 */
    OP_FIXPCK_LG2(3), /* REPEATED FIXED64 */
    OP_FIXPCK_LG2(2), /* REPEATED FIXED32 */
    OP_VARPCK_LG2(0), /* REPEATED BOOL */
    OP_STRING,        /* REPEATED STRING */
    OP_SUBMSG,        /* REPEATED GROUP */
    OP_SUBMSG,        /* REPEATED MESSAGE */
    OP_BYTES,         /* REPEATED BYTES */
    OP_VARPCK_LG2(2), /* REPEATED UINT32 */
    OP_VARPCK_LG2(2), /* REPEATED ENUM */
    OP_FIXPCK_LG2(2), /* REPEATED SFIXED32 */
    OP_FIXPCK_LG2(3), /* REPEATED SFIXED64 */
    OP_VARPCK_LG2(2), /* REPEATED SINT32 */
    OP_VARPCK_LG2(3), /* REPEATED SINT64 */
};

/* Data pertaining to the parse. */
typedef struct {
  const char *end;         /* Can read up to 16 bytes slop beyond this. */
  const char *limit_ptr;   /* = end + UPB_MIN(limit, 0) */
  int limit;               /* Submessage limit relative to end. */
  int depth;
  uint32_t end_group; /* Set to field number of END_GROUP tag, if any. */
  bool alias;
  char patch[32];
  upb_arena arena;
  jmp_buf err;
} upb_decstate;

typedef union {
  bool bool_val;
  uint32_t uint32_val;
  uint64_t uint64_val;
  uint32_t size;
} wireval;

static const char *decode_msg(upb_decstate *d, const char *ptr, upb_msg *msg,
                              const upb_msglayout *layout);

UPB_NORETURN static void decode_err(upb_decstate *d) { longjmp(d->err, 1); }

void decode_verifyutf8(upb_decstate *d, const char *buf, int len) {
  static const uint8_t utf8_offset[] = {
      1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
      1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
      1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
      1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
      1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
      1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
      2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
      4, 4, 4, 4, 4, 4, 4, 4, 0, 0, 0, 0, 0, 0, 0, 0,
  };

  int i, j;
  uint8_t offset;

  i = 0;
  while (i < len) {
    offset = utf8_offset[(uint8_t)buf[i]];
    if (offset == 0 || i + offset > len) {
      decode_err(d);
    }
    for (j = i + 1; j < i + offset; j++) {
      if ((buf[j] & 0xc0) != 0x80) {
        decode_err(d);
      }
    }
    i += offset;
  }
  if (i != len) decode_err(d);
}

static bool decode_reserve(upb_decstate *d, upb_array *arr, size_t elem) {
  bool need_realloc = arr->size - arr->len < elem;
  if (need_realloc && !_upb_array_realloc(arr, arr->len + elem, &d->arena)) {
    decode_err(d);
  }
  return need_realloc;
}

typedef struct {
  const char *ptr;
  uint64_t val;
} decode_vret;

UPB_NOINLINE
static decode_vret decode_longvarint64(const char *ptr, uint64_t val) {
  decode_vret ret = {NULL, 0};
  uint64_t byte;
  int i;
  for (i = 1; i < 10; i++) {
    byte = (uint8_t)ptr[i];
    val += (byte - 1) << (i * 7);
    if (!(byte & 0x80)) {
      ret.ptr = ptr + i + 1;
      ret.val = val;
      return ret;
    }
  }
  return ret;
}

UPB_FORCEINLINE
static const char *decode_varint64(upb_decstate *d, const char *ptr,
                                   uint64_t *val) {
  uint64_t byte = (uint8_t)*ptr;
  if (UPB_LIKELY((byte & 0x80) == 0)) {
    *val = byte;
    return ptr + 1;
  } else {
    decode_vret res = decode_longvarint64(ptr, byte);
    if (!res.ptr) decode_err(d);
    *val = res.val;
    return res.ptr;
  }
}

UPB_FORCEINLINE
static const char *decode_varint32(upb_decstate *d, const char *ptr,
                                   uint32_t *val) {
  uint64_t u64;
  ptr = decode_varint64(d, ptr, &u64);
  if (u64 > UINT32_MAX) decode_err(d);
  *val = (uint32_t)u64;
  return ptr;
}

static void decode_munge(int type, wireval *val) {
  switch (type) {
    case UPB_DESCRIPTOR_TYPE_BOOL:
      val->bool_val = val->uint64_val != 0;
      break;
    case UPB_DESCRIPTOR_TYPE_SINT32: {
      uint32_t n = val->uint32_val;
      val->uint32_val = (n >> 1) ^ -(int32_t)(n & 1);
      break;
    }
    case UPB_DESCRIPTOR_TYPE_SINT64: {
      uint64_t n = val->uint64_val;
      val->uint64_val = (n >> 1) ^ -(int64_t)(n & 1);
      break;
    }
    case UPB_DESCRIPTOR_TYPE_INT32:
    case UPB_DESCRIPTOR_TYPE_UINT32:
      if (!_upb_isle()) {
        /* The next stage will memcpy(dst, &val, 4) */
        val->uint32_val = val->uint64_val;
      }
      break;
  }
}

static const upb_msglayout_field *upb_find_field(const upb_msglayout *l,
                                                 uint32_t field_number) {
  static upb_msglayout_field none = {0, 0, 0, 0, 0, 0};

  /* Lots of optimization opportunities here. */
  int i;
  if (l == NULL) return &none;
  for (i = 0; i < l->field_count; i++) {
    if (l->fields[i].number == field_number) {
      return &l->fields[i];
    }
  }

  return &none; /* Unknown field. */
}

static upb_msg *decode_newsubmsg(upb_decstate *d, const upb_msglayout *layout,
                                 const upb_msglayout_field *field) {
  const upb_msglayout *subl = layout->submsgs[field->submsg_index];
  return _upb_msg_new_inl(subl, &d->arena);
}

static int decode_pushlimit(upb_decstate *d, const char *ptr, int size) {
  int limit = size + (int)(ptr - d->end);
  int delta = d->limit - limit;
  d->limit = limit;
  d->limit_ptr = d->end + UPB_MIN(0, limit);
  return delta;
}

static void decode_poplimit(upb_decstate *d, int saved_delta) {
  d->limit += saved_delta;
  d->limit_ptr = d->end + UPB_MIN(0, d->limit);
}

typedef struct {
  bool ok;
  const char *ptr;
} decode_doneret;

UPB_NOINLINE
static const char *decode_isdonefallback(upb_decstate *d, const char *ptr,
                                         int overrun) {
  if (overrun < d->limit) {
    /* Need to copy remaining data into patch buffer. */
    UPB_ASSERT(overrun < 16);
    memset(d->patch + 16, 0, 16);
    memcpy(d->patch, d->end, 16);
    ptr = &d->patch[0] + overrun;
    d->end = &d->patch[16];
    d->limit -= 16;
    d->limit_ptr = d->end + d->limit;
    d->alias = false;
    UPB_ASSERT(ptr < d->limit_ptr);
    return ptr;
  } else {
    decode_err(d);
  }
}

UPB_FORCEINLINE
static bool decode_isdone(upb_decstate *d, const char **ptr) {
  int overrun = *ptr - d->end;
  if (UPB_LIKELY(*ptr < d->limit_ptr)) {
    return false;
  } else if (UPB_LIKELY(overrun == d->limit)) {
    return true;
  } else {
    *ptr = decode_isdonefallback(d, *ptr, overrun);
    return false;
  }
}

static const char *decode_readstr(upb_decstate *d, const char *ptr, int size,
                                  upb_strview *str) {
  if (d->alias) {
    str->data = ptr;
  } else {
    char *data =  upb_arena_malloc(&d->arena, size);
    if (!data) decode_err(d);
    memcpy(data, ptr, size);
    str->data = data;
  }
  str->size = size;
  return ptr + size;
}

static const char *decode_tosubmsg(upb_decstate *d, const char *ptr,
                                   upb_msg *submsg, const upb_msglayout *layout,
                                   const upb_msglayout_field *field, int size) {
  const upb_msglayout *subl = layout->submsgs[field->submsg_index];
  int saved_delta = decode_pushlimit(d, ptr, size);
  if (--d->depth < 0) decode_err(d);
  ptr = decode_msg(d, ptr, submsg, subl);
  decode_poplimit(d, saved_delta);
  if (d->end_group != 0) decode_err(d);
  d->depth++;
  return ptr;
}

static const char *decode_group(upb_decstate *d, const char *ptr,
                                upb_msg *submsg, const upb_msglayout *subl,
                                uint32_t number) {
  if (--d->depth < 0) decode_err(d);
  ptr = decode_msg(d, ptr, submsg, subl);
  if (d->end_group != number) decode_err(d);
  d->end_group = 0;
  d->depth++;
  return ptr;
}

static const char *decode_togroup(upb_decstate *d, const char *ptr,
                                  upb_msg *submsg, const upb_msglayout *layout,
                                  const upb_msglayout_field *field) {
  const upb_msglayout *subl = layout->submsgs[field->submsg_index];
  return decode_group(d, ptr, submsg, subl, field->number);
}

static const char *decode_toarray(upb_decstate *d, const char *ptr,
                                  upb_msg *msg, const upb_msglayout *layout,
                                  const upb_msglayout_field *field, wireval val,
                                  int op) {
  upb_array **arrp = UPB_PTR_AT(msg, field->offset, void);
  upb_array *arr = *arrp;
  void *mem;

  if (arr) {
    decode_reserve(d, arr, 1);
  } else {
    size_t lg2 = desctype_to_elem_size_lg2[field->descriptortype];
    arr = _upb_array_new(&d->arena, 4, lg2);
    if (!arr) decode_err(d);
    *arrp = arr;
  }

  switch (op) {
    case OP_SCALAR_LG2(0):
    case OP_SCALAR_LG2(2):
    case OP_SCALAR_LG2(3):
      /* Append scalar value. */
      mem = UPB_PTR_AT(_upb_array_ptr(arr), arr->len << op, void);
      arr->len++;
      memcpy(mem, &val, 1 << op);
      return ptr;
    case OP_STRING:
      decode_verifyutf8(d, ptr, val.size);
      /* Fallthrough. */
    case OP_BYTES: {
      /* Append bytes. */
      upb_strview *str = (upb_strview*)_upb_array_ptr(arr) + arr->len;
      arr->len++;
      return decode_readstr(d, ptr, val.size, str);
    }
    case OP_SUBMSG: {
      /* Append submessage / group. */
      upb_msg *submsg = decode_newsubmsg(d, layout, field);
      *UPB_PTR_AT(_upb_array_ptr(arr), arr->len * sizeof(void *), upb_msg *) =
          submsg;
      arr->len++;
      if (UPB_UNLIKELY(field->descriptortype == UPB_DTYPE_GROUP)) {
        return decode_togroup(d, ptr, submsg, layout, field);
      } else {
        return decode_tosubmsg(d, ptr, submsg, layout, field, val.size);
      }
    }
    case OP_FIXPCK_LG2(2):
    case OP_FIXPCK_LG2(3): {
      /* Fixed packed. */
      int lg2 = op - OP_FIXPCK_LG2(0);
      int mask = (1 << lg2) - 1;
      size_t count = val.size >> lg2;
      if ((val.size & mask) != 0) {
        decode_err(d); /* Length isn't a round multiple of elem size. */
      }
      decode_reserve(d, arr, count);
      mem = UPB_PTR_AT(_upb_array_ptr(arr), arr->len << lg2, void);
      arr->len += count;
      memcpy(mem, ptr, val.size);  /* XXX: ptr boundary. */
      return ptr + val.size;
    }
    case OP_VARPCK_LG2(0):
    case OP_VARPCK_LG2(2):
    case OP_VARPCK_LG2(3): {
      /* Varint packed. */
      int lg2 = op - OP_VARPCK_LG2(0);
      int scale = 1 << lg2;
      int saved_limit = decode_pushlimit(d, ptr, val.size);
      char *out = UPB_PTR_AT(_upb_array_ptr(arr), arr->len << lg2, void);
      while (!decode_isdone(d, &ptr)) {
        wireval elem;
        ptr = decode_varint64(d, ptr, &elem.uint64_val);
        decode_munge(field->descriptortype, &elem);
        if (decode_reserve(d, arr, 1)) {
          out = UPB_PTR_AT(_upb_array_ptr(arr), arr->len << lg2, void);
        }
        arr->len++;
        memcpy(out, &elem, scale);
        out += scale;
      }
      decode_poplimit(d, saved_limit);
      return ptr;
    }
    default:
      UPB_UNREACHABLE();
  }
}

static const char *decode_tomap(upb_decstate *d, const char *ptr, upb_msg *msg,
                                const upb_msglayout *layout,
                                const upb_msglayout_field *field, wireval val) {
  upb_map **map_p = UPB_PTR_AT(msg, field->offset, upb_map *);
  upb_map *map = *map_p;
  upb_map_entry ent;
  const upb_msglayout *entry = layout->submsgs[field->submsg_index];

  if (!map) {
    /* Lazily create map. */
    const upb_msglayout *entry = layout->submsgs[field->submsg_index];
    const upb_msglayout_field *key_field = &entry->fields[0];
    const upb_msglayout_field *val_field = &entry->fields[1];
    char key_size = desctype_to_mapsize[key_field->descriptortype];
    char val_size = desctype_to_mapsize[val_field->descriptortype];
    UPB_ASSERT(key_field->offset == 0);
    UPB_ASSERT(val_field->offset == sizeof(upb_strview));
    map = _upb_map_new(&d->arena, key_size, val_size);
    *map_p = map;
  }

  /* Parse map entry. */
  memset(&ent, 0, sizeof(ent));

  if (entry->fields[1].descriptortype == UPB_DESCRIPTOR_TYPE_MESSAGE ||
      entry->fields[1].descriptortype == UPB_DESCRIPTOR_TYPE_GROUP) {
    /* Create proactively to handle the case where it doesn't appear. */
    ent.v.val = upb_value_ptr(_upb_msg_new(entry->submsgs[0], &d->arena));
  }

  ptr = decode_tosubmsg(d, ptr, &ent.k, layout, field, val.size);
  _upb_map_set(map, &ent.k, map->key_size, &ent.v, map->val_size, &d->arena);
  return ptr;
}

static const char *decode_tomsg(upb_decstate *d, const char *ptr, upb_msg *msg,
                                const upb_msglayout *layout,
                                const upb_msglayout_field *field, wireval val,
                                int op) {
  void *mem = UPB_PTR_AT(msg, field->offset, void);
  int type = field->descriptortype;

  /* Set presence if necessary. */
  if (field->presence < 0) {
    /* Oneof case */
    uint32_t *oneof_case = _upb_oneofcase_field(msg, field);
    if (op == OP_SUBMSG && *oneof_case != field->number) {
      memset(mem, 0, sizeof(void*));
    }
    *oneof_case = field->number;
  } else if (field->presence > 0) {
    _upb_sethas_field(msg, field);
  }

  /* Store into message. */
  switch (op) {
    case OP_SUBMSG: {
      upb_msg **submsgp = mem;
      upb_msg *submsg = *submsgp;
      if (!submsg) {
        submsg = decode_newsubmsg(d, layout, field);
        *submsgp = submsg;
      }
      if (UPB_UNLIKELY(type == UPB_DTYPE_GROUP)) {
        ptr = decode_togroup(d, ptr, submsg, layout, field);
      } else {
        ptr = decode_tosubmsg(d, ptr, submsg, layout, field, val.size);
      }
      break;
    }
    case OP_STRING:
      decode_verifyutf8(d, ptr, val.size);
      /* Fallthrough. */
    case OP_BYTES:
      return decode_readstr(d, ptr, val.size, mem);
    case OP_SCALAR_LG2(3):
      memcpy(mem, &val, 8);
      break;
    case OP_SCALAR_LG2(2):
      memcpy(mem, &val, 4);
      break;
    case OP_SCALAR_LG2(0):
      memcpy(mem, &val, 1);
      break;
    default:
      UPB_UNREACHABLE();
  }

  return ptr;
}

static const char *decode_msg(upb_decstate *d, const char *ptr, upb_msg *msg,
                              const upb_msglayout *layout) {
  while (!decode_isdone(d, &ptr)) {
    uint32_t tag;
    const upb_msglayout_field *field;
    int field_number;
    int wire_type;
    const char *field_start = ptr;
    wireval val;
    int op;

    ptr = decode_varint32(d, ptr, &tag);
    field_number = tag >> 3;
    wire_type = tag & 7;

    field = upb_find_field(layout, field_number);

    switch (wire_type) {
      case UPB_WIRE_TYPE_VARINT:
        ptr = decode_varint64(d, ptr, &val.uint64_val);
        op = varint_ops[field->descriptortype];
        decode_munge(field->descriptortype, &val);
        break;
      case UPB_WIRE_TYPE_32BIT:
        memcpy(&val.uint32_val, ptr, 4);
        val.uint32_val = _upb_be_swap32(val.uint32_val);
        ptr += 4;
        op = OP_SCALAR_LG2(2);
        if (((1 << field->descriptortype) & fixed32_ok) == 0) goto unknown;
        break;
      case UPB_WIRE_TYPE_64BIT:
        memcpy(&val.uint64_val, ptr, 8);
        val.uint64_val = _upb_be_swap64(val.uint64_val);
        ptr += 8;
        op = OP_SCALAR_LG2(3);
        if (((1 << field->descriptortype) & fixed64_ok) == 0) goto unknown;
        break;
      case UPB_WIRE_TYPE_DELIMITED: {
        int ndx = field->descriptortype;
        if (_upb_isrepeated(field)) ndx += 18;
        ptr = decode_varint32(d, ptr, &val.size);
        if (val.size >= INT32_MAX || ptr - d->end + val.size > d->limit) {
          decode_err(d); /* Length overflow. */
        }
        op = delim_ops[ndx];
        break;
      }
      case UPB_WIRE_TYPE_START_GROUP:
        val.uint32_val = field_number;
        op = OP_SUBMSG;
        if (field->descriptortype != UPB_DTYPE_GROUP) goto unknown;
        break;
      case UPB_WIRE_TYPE_END_GROUP:
        d->end_group = field_number;
        return ptr;
      default:
        decode_err(d);
    }

    if (op >= 0) {
      /* Parse, using op for dispatch. */
      switch (field->label) {
        case UPB_LABEL_REPEATED:
        case _UPB_LABEL_PACKED:
          ptr = decode_toarray(d, ptr, msg, layout, field, val, op);
          break;
        case _UPB_LABEL_MAP:
          ptr = decode_tomap(d, ptr, msg, layout, field, val);
          break;
        default:
          ptr = decode_tomsg(d, ptr, msg, layout, field, val, op);
          break;
      }
    } else {
    unknown:
      /* Skip unknown field. */
      if (field_number == 0) decode_err(d);
      if (wire_type == UPB_WIRE_TYPE_START_GROUP) {
        ptr = decode_group(d, ptr, NULL, NULL, field_number);
      }
      if (msg) {
        if (wire_type == UPB_WIRE_TYPE_DELIMITED) ptr += val.size;
        if (!_upb_msg_addunknown(msg, field_start, ptr - field_start,
                                 &d->arena)) {
          decode_err(d);
        }
      }
    }
  }

  return ptr;
}

bool upb_decode(const char *buf, size_t size, void *msg, const upb_msglayout *l,
                upb_arena *arena) {
  bool ok;
  upb_decstate state;

  if (size == 0) {
    return true;
  } else if (size < 16) {
    memset(&state.patch, 0, 32);
    memcpy(&state.patch, buf, size);
    buf = state.patch;
    state.end = buf + size;
    state.limit = 0;
    state.alias = false;
  } else {
    state.end = buf + size - 16;
    state.limit = 16;
    state.alias = true;
  }

  state.limit_ptr = state.end;
  state.depth = 64;
  state.end_group = 0;
  state.arena.head = arena->head;
  state.arena.last_size = arena->last_size;
  state.arena.parent = arena;

  if (UPB_UNLIKELY(setjmp(state.err))) {
    ok = false;
  } else {
    decode_msg(&state, buf, msg, l);
    ok = state.end_group == 0;
  }

  arena->head.ptr = state.arena.head.ptr;
  arena->head.end = state.arena.head.end;
  return ok;
}

#undef OP_SCALAR_LG2
#undef OP_FIXPCK_LG2
#undef OP_VARPCK_LG2
#undef OP_STRING
#undef OP_SUBMSG
