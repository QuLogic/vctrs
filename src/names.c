#include <ctype.h>

#include "vctrs.h"
#include "utils.h"

#include <ctype.h>

static void describe_repair(SEXP old_names, SEXP new_names);

// 3 leading '.' + 1 trailing '\0' + 24 characters
#define MAX_IOTA_SIZE 28

// Initialised at load time
SEXP syms_as_universal_names = NULL;
SEXP syms_validate_unique_names = NULL;
SEXP fns_as_universal_names = NULL;
SEXP fns_validate_unique_names = NULL;

// Defined below
SEXP vctrs_as_minimal_names(SEXP names);
SEXP vec_as_universal_names(SEXP names, bool quiet);
SEXP vec_validate_unique_names(SEXP names);


// [[ include("vctrs.h") ]]
SEXP vec_as_names(SEXP names, enum name_repair_arg type, bool quiet) {
  switch (type) {
  case name_repair_none: return names;
  case name_repair_minimal: return vctrs_as_minimal_names(names);
  case name_repair_unique: return vec_as_unique_names(names, quiet);
  case name_repair_universal: return vec_as_universal_names(names, quiet);
  case name_repair_check_unique: return vec_validate_unique_names(names);
  }
  never_reached("vec_as_names");
}

SEXP vec_as_universal_names(SEXP names, bool quiet) {
  SEXP quiet_obj = PROTECT(r_lgl(quiet));
  SEXP out = vctrs_dispatch2(syms_as_universal_names, fns_as_universal_names,
                             syms_names, names,
                             syms_quiet, quiet_obj);
  UNPROTECT(1);
  return out;
}
SEXP vec_validate_unique_names(SEXP names) {
  SEXP out = PROTECT(vctrs_dispatch1(syms_validate_unique_names, fns_validate_unique_names,
                                     syms_names, names));

  // Restore visibility
  Rf_eval(R_NilValue, R_EmptyEnv);

  UNPROTECT(1);
  return out;
}


// [[ register(); include("vctrs.h") ]]
SEXP vec_names(SEXP x) {
  if (OBJECT(x) && Rf_inherits(x, "data.frame")) {
    return R_NilValue;
  }

  if (vec_dim_n(x) == 1) {
    if (OBJECT(x)) {
      return vctrs_dispatch1(syms_names, fns_names, syms_x, x);
    } else {
      return r_names(x);
    }
  }

  SEXP dimnames = PROTECT(Rf_getAttrib(x, R_DimNamesSymbol));
  if (dimnames == R_NilValue || Rf_length(dimnames) < 1) {
    UNPROTECT(1);
    return R_NilValue;
  }

  SEXP out = VECTOR_ELT(dimnames, 0);
  UNPROTECT(1);
  return out;
}

// [[ register() ]]
SEXP vctrs_as_minimal_names(SEXP names) {
  if (TYPEOF(names) != STRSXP) {
    Rf_errorcall(R_NilValue, "`names` must be a character vector");
  }

  R_len_t i = 0;
  R_len_t n = Rf_length(names);
  const SEXP* ptr = STRING_PTR_RO(names);

  for (; i < n; ++i, ++ptr) {
    SEXP elt = *ptr;
    if (elt == NA_STRING) {
      break;
    }
  }
  if (i == n) {
    return names;
  }

  names = PROTECT(Rf_shallow_duplicate(names));

  for (; i < n; ++i, ++ptr) {
    SEXP elt = *ptr;
    if (elt == NA_STRING) {
      SET_STRING_ELT(names, i, strings_empty);
    }
  }

  UNPROTECT(1);
  return names;
}

// [[ register() ]]
SEXP vctrs_minimal_names(SEXP x) {
  SEXP names = PROTECT(vec_names(x));

  if (names == R_NilValue) {
    names = Rf_allocVector(STRSXP, vec_size(x));
  } else {
    names = vctrs_as_minimal_names(names);
  }

  UNPROTECT(1);
  return names;
}


// From dictionary.c
SEXP vctrs_duplicated(SEXP x);

static bool any_has_suffix(SEXP names);
static SEXP as_unique_names_impl(SEXP names, bool quiet);
static void stop_large_name();
static bool is_dotdotint(const char* name);
static ptrdiff_t suffix_pos(const char* name);
static bool needs_suffix(SEXP str);

// [[ include("vctrs.h") ]]
SEXP vec_as_unique_names(SEXP names, bool quiet) {
  if (is_unique_names(names) && !any_has_suffix(names)) {
    return names;
  } else {
    return(as_unique_names_impl(names, quiet));
  }
}

// [[ include("vctrs.h") ]]
bool is_unique_names(SEXP names) {
  if (TYPEOF(names) != STRSXP) {
    Rf_errorcall(R_NilValue, "`names` must be a character vector");
  }

  R_len_t n = Rf_length(names);
  const SEXP* names_ptr = STRING_PTR_RO(names);

  if (duplicated_any(names)) {
    return false;
  }

  for (R_len_t i = 0; i < n; ++i) {
    SEXP elt = names_ptr[i];

    if (needs_suffix(elt)) {
      return false;
    }
  }

  return true;
}

bool any_has_suffix(SEXP names) {
  R_len_t n = Rf_length(names);
  const SEXP* names_ptr = STRING_PTR_RO(names);

  for (R_len_t i = 0; i < n; ++i) {
    SEXP elt = names_ptr[i];

    if (suffix_pos(CHAR(elt)) >= 0) {
      return true;
    }
  }

  return false;
}

SEXP as_unique_names_impl(SEXP names, bool quiet) {
  R_len_t n = Rf_length(names);

  SEXP new_names = PROTECT(Rf_shallow_duplicate(names));
  const SEXP* new_names_ptr = STRING_PTR_RO(new_names);

  for (R_len_t i = 0; i < n; ++i) {
    SEXP elt = new_names_ptr[i];

    // Set `NA` and dots values to "" so they get replaced by `...n`
    // later on
    if (needs_suffix(elt)) {
      elt = strings_empty;
      SET_STRING_ELT(new_names, i, elt);
      continue;
    }

    // Strip `...n` suffixes
    const char* nm = CHAR(elt);
    int pos = suffix_pos(nm);
    if (pos >= 0) {
      elt = Rf_mkCharLenCE(nm, pos, Rf_getCharCE(elt));
      SET_STRING_ELT(new_names, i, elt);
      continue;
    }
  }

  // Append all duplicates with a suffix

  SEXP dups = PROTECT(vctrs_duplicated(new_names));
  const int* dups_ptr = LOGICAL_RO(dups);

  for (R_len_t i = 0; i < n; ++i) {
    SEXP elt = new_names_ptr[i];

    if (elt != strings_empty && !dups_ptr[i]) {
      continue;
    }

    const char* name = CHAR(elt);

    int size = strlen(name);
    int buf_size = size + MAX_IOTA_SIZE;

    R_CheckStack2(buf_size);
    char buf[buf_size];
    buf[0] = '\0';

    memcpy(buf, name, size);
    int remaining = buf_size - size;

    int needed = snprintf(buf + size, remaining, "...%d", i + 1);
    if (needed >= remaining) {
      stop_large_name();
    }

    SET_STRING_ELT(new_names, i, Rf_mkCharLenCE(buf, size + needed, Rf_getCharCE(elt)));
  }

  if (!quiet) {
    describe_repair(names, new_names);
  }

  UNPROTECT(2);
  return new_names;
}

SEXP vctrs_as_unique_names(SEXP names, SEXP quiet) {
  SEXP out = PROTECT(vec_as_unique_names(names, LOGICAL(quiet)[0]));
  UNPROTECT(1);
  return out;
}

SEXP vctrs_is_unique_names(SEXP names) {
  bool out = is_unique_names(names);
  return Rf_ScalarLogical(out);
}

static bool is_dotdotint(const char* name) {
  int n = strlen(name);

  if (n < 3) {
    return false;
  }
  if (name[0] != '.' || name[1] != '.') {
    return false;
  }

  if (name[2] == '.') {
    name += 3;
  } else {
    name += 2;
  }

  return (bool) strtol(name, NULL, 10);
}

static ptrdiff_t suffix_pos(const char* name) {
  int n = strlen(name);

  const char* suffix_end = NULL;
  int in_dots = 0;
  bool in_digits = false;

  for (const char* ptr = name + n - 1; ptr >= name; --ptr) {
    char c = *ptr;

    if (in_digits) {
      if (c == '.') {
        in_digits = false;
        in_dots = 1;
        continue;
      }

      if (isdigit(c)) {
        continue;
      }

      goto done;
    }

    switch (in_dots) {
    case 0:
      if (isdigit(c)) {
        in_digits = true;
        continue;
      }
      goto done;
    case 1:
    case 2:
      if (c == '.') {
        ++in_dots;
        continue;
      }
      goto done;
    case 3:
      suffix_end = ptr + 1;
      if (isdigit(c)) {
        in_dots = 0;
        in_digits = true;
        continue;
      }
      goto done;

    default:
      Rf_error("Internal error: Unexpected state in `suffix_pos()`");
    }}

 done:
  if (suffix_end) {
    return suffix_end - name;
  } else {
    return -1;
  }
}

static void stop_large_name() {
  Rf_errorcall(R_NilValue, "Can't tidy up name because it is too large");
}

static bool needs_suffix(SEXP str) {
  return
    str == NA_STRING ||
    str == strings_dots ||
    str == strings_empty ||
    is_dotdotint(CHAR(str));
}


static SEXP names_iota(R_len_t n);
static SEXP vec_unique_names_impl(SEXP names, R_len_t n, bool quiet);

// [[ register() ]]
SEXP vctrs_unique_names(SEXP x, SEXP quiet) {
  return vec_unique_names(x, LOGICAL(quiet)[0]);
}

// [[ include("utils.h") ]]
SEXP vec_unique_names(SEXP x, bool quiet) {
  SEXP names = PROTECT(Rf_getAttrib(x, R_NamesSymbol));
  SEXP out = vec_unique_names_impl(names, vec_size(x), quiet);
  UNPROTECT(1);
  return out;
}
// [[ include("utils.h") ]]
SEXP vec_unique_colnames(SEXP x, bool quiet) {
  SEXP names = PROTECT(colnames(x));
  SEXP out = vec_unique_names_impl(names, Rf_ncols(x), quiet);
  UNPROTECT(1);
  return out;
}

static SEXP vec_unique_names_impl(SEXP names, R_len_t n, bool quiet) {
  SEXP out;
  if (names == R_NilValue) {
    out = PROTECT(names_iota(n));
    if (!quiet) {
      describe_repair(names, out);
    }
  } else {
    out = PROTECT(vec_as_unique_names(names, quiet));
  }

  UNPROTECT(1);
  return(out);
}

static SEXP names_iota(R_len_t n) {
  char buf[MAX_IOTA_SIZE];
  SEXP nms = r_chr_iota(n, buf, MAX_IOTA_SIZE, "...");

  if (nms == R_NilValue) {
    Rf_errorcall(R_NilValue, "Too many names to repair.");
  }

  return nms;
}



static void describe_repair(SEXP old_names, SEXP new_names) {
  SEXP call = PROTECT(Rf_lang3(Rf_install("describe_repair"),
    old_names, new_names));
  Rf_eval(call, vctrs_ns_env);

  // To reset visibility when called from a `.External2()`
  Rf_eval(R_NilValue, R_EmptyEnv);

  UNPROTECT(1);
}


static SEXP outer_names_cat(const char* outer, SEXP names);
static SEXP outer_names_seq(const char* outer, R_len_t n);

// [[ register() ]]
SEXP vctrs_outer_names(SEXP names, SEXP outer, SEXP n) {
  if (names != R_NilValue && TYPEOF(names) != STRSXP) {
    Rf_error("Internal error: `names` must be `NULL` or a string");
  }
  if (!r_is_number(n)) {
    Rf_error("Internal error: `n` must be a single integer");
  }

  if (outer != R_NilValue) {
    outer = r_chr_get(outer, 0);
  }

  return outer_names(names, outer, r_int_get(n, 0));
}

// [[ include("utils.h") ]]
SEXP outer_names(SEXP names, SEXP outer, R_len_t n) {
  if (outer == R_NilValue) {
    return names;
  }
  if (TYPEOF(outer) != CHARSXP) {
    Rf_error("Internal error: `outer` must be a scalar string.");
  }

  if (outer == strings_empty || outer == NA_STRING) {
    return names;
  }

  if (r_is_empty_names(names)) {
    if (n == 1) {
      return r_str_as_character(outer);
    } else {
      return outer_names_seq(CHAR(outer), n);
    }
  } else {
    return outer_names_cat(CHAR(outer), names);
  }
}

// [[ register() ]]
SEXP vctrs_apply_name_spec(SEXP name_spec, SEXP outer, SEXP inner, SEXP n) {
  return apply_name_spec(name_spec, r_chr_get(outer, 0), inner, r_int_get(n, 0));
}

static SEXP glue_as_name_spec(SEXP spec);

// [[ include("utils.h") ]]
SEXP apply_name_spec(SEXP name_spec, SEXP outer, SEXP inner, R_len_t n) {
  if (outer == R_NilValue) {
    return inner;
  }
  if (TYPEOF(outer) != CHARSXP) {
    Rf_error("Internal error: `outer` must be a scalar string.");
  }

  if (outer == strings_empty || outer == NA_STRING) {
    return inner;
  }

  if (r_is_empty_names(inner)) {
    if (n == 1) {
      return r_str_as_character(outer);
    }
    inner = PROTECT(r_seq(1, n + 1));
  } else {
    inner = PROTECT(inner);
  }

  switch (TYPEOF(name_spec)) {
  case CLOSXP:
    break;
  case STRSXP:
    name_spec = glue_as_name_spec(name_spec);
    break;
  default:
    name_spec = r_as_function(name_spec, ".name_spec");
    break;
  case NILSXP:
    Rf_errorcall(R_NilValue,
                 "Can't merge the outer name `%s` with a vector of length > 1.\n"
                 "Please supply a `.name_spec` specification.",
                 CHAR(outer));
  }
  PROTECT(name_spec);

  // Recycle `outer` so specs don't need to refer to both `outer` and `inner`
  SEXP outer_chr = PROTECT(Rf_allocVector(STRSXP, n));
  r_chr_fill(outer_chr, outer, n);

  SEXP out = vctrs_dispatch2(syms_dot_name_spec, name_spec,
                             syms_outer, outer_chr,
                             syms_inner, inner);

  if (TYPEOF(out) != STRSXP) {
    Rf_errorcall(R_NilValue, "`.name_spec` must return a character vector.");
  }
  if (Rf_length(out) != n) {
    Rf_errorcall(R_NilValue, "`.name_spec` must return a character vector as long as `inner`.");
  }

  UNPROTECT(3);
  return out;
}


static SEXP syms_glue_as_name_spec = NULL;
static SEXP fns_glue_as_name_spec = NULL;
static SEXP syms_internal_spec = NULL;

static SEXP glue_as_name_spec(SEXP spec) {
  if (!r_is_string(spec)) {
    Rf_errorcall(R_NilValue, "Glue specification in `.name_spec` must be a single string.");
  }
  return vctrs_dispatch1(syms_glue_as_name_spec, fns_glue_as_name_spec,
                         syms_internal_spec, spec);
}


static SEXP outer_names_cat(const char* outer, SEXP names) {
  names = PROTECT(Rf_shallow_duplicate(names));
  R_len_t n = Rf_length(names);

  int outer_len = strlen(outer);
  int names_len = r_chr_max_len(names);

  int total_len = outer_len + names_len + strlen("..") + 1;

  R_CheckStack2(total_len);
  char buf[total_len];
  buf[total_len - 1] = '\0';
  char* bufp = buf;

  memcpy(bufp, outer, outer_len); bufp += outer_len;
  *bufp = '.'; bufp += 1;
  *bufp = '.'; bufp += 1;

  SEXP* p = STRING_PTR(names);

  for (R_len_t i = 0; i < n; ++i, ++p) {
    const char* inner = CHAR(*p);
    int inner_n = strlen(inner);

    memcpy(bufp, inner, inner_n);
    bufp[inner_n] = '\0';

    SET_STRING_ELT(names, i, r_str(buf));
  }

  UNPROTECT(1);
  return names;
}

static SEXP outer_names_seq(const char* outer, R_len_t n) {
  int total_len = 24 + strlen(outer) + 1;

  R_CheckStack2(total_len);
  char buf[total_len];

  return r_chr_iota(n, buf, total_len, outer);
}


// Initialised at load time
SEXP syms_set_rownames = NULL;
SEXP fns_set_rownames = NULL;

// [[ include("utils.h") ]]
SEXP set_rownames(SEXP x, SEXP names) {
  return vctrs_dispatch2(syms_set_rownames, fns_set_rownames,
                         syms_x, x,
                         syms_names, names);
}


enum name_repair_arg validate_name_repair(SEXP arg) {
  if (!Rf_length(arg)) {
    Rf_errorcall(R_NilValue, "`.name_repair` must be a string. See `?vctrs::vec_as_names`.");
  }

  arg = r_chr_get(arg, 0);

  if (arg == strings_none) {
    return name_repair_none;
  }
  if (arg == strings_minimal) {
    return name_repair_minimal;
  }
  if (arg == strings_unique) {
    return name_repair_unique;
  }
  if (arg == strings_universal) {
    return name_repair_universal;
  }
  if (arg == strings_check_unique) {
    return name_repair_check_unique;
  }

  Rf_errorcall(R_NilValue, "`.name_repair` can't be \"%s\". See `?vctrs::vec_as_names`.", CHAR(arg));
}

// [[ include("vctrs.h") ]]
const char* name_repair_arg_as_c_string(enum name_repair_arg arg) {
  switch (arg) {
  case name_repair_none: return "none";
  case name_repair_minimal: return "minimal";
  case name_repair_unique: return "unique";
  case name_repair_universal: return "universal";
  case name_repair_check_unique: return "check_unique";
  }
  never_reached("name_repair_arg_as_c_string");
}


void vctrs_init_names(SEXP ns) {
  syms_set_rownames = Rf_install("set_rownames");
  syms_as_universal_names = Rf_install("as_universal_names");
  syms_validate_unique_names = Rf_install("validate_unique");

  fns_set_rownames = r_env_get(ns, syms_set_rownames);
  fns_as_universal_names = r_env_get(ns, syms_as_universal_names);
  fns_validate_unique_names = r_env_get(ns, syms_validate_unique_names);

  syms_glue_as_name_spec = Rf_install("glue_as_name_spec");
  fns_glue_as_name_spec = r_env_get(ns, syms_glue_as_name_spec);
  syms_internal_spec = Rf_install("_spec");
}
