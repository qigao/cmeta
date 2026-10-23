#ifndef SALTS_ASN1_API_H
#define SALTS_ASN1_API_H

#include "cmeta_api.h"

/* Separate DLL boundary for the standalone Salts::Asn1 target. */
#ifndef SALTS_ASN1_API
#  if defined(_WIN32)
#    if defined(SALTS_ASN1_BUILD)
#      define SALTS_ASN1_API __declspec(dllexport)
#    else
#      define SALTS_ASN1_API __declspec(dllimport)
#    endif
#  elif defined(__GNUC__) && __GNUC__ >= 4
#    define SALTS_ASN1_API __attribute__((visibility("default")))
#  else
#    define SALTS_ASN1_API
#  endif
#endif

#ifdef __cplusplus
#  define SALTS_ASN1_C_API extern "C" SALTS_ASN1_API
#else
#  define SALTS_ASN1_C_API SALTS_ASN1_API
#endif

/* Source compatibility for clients migrating from SaltsNet 1.x. */
#ifndef SALTSNET_ASN1_C_API
#  define SALTSNET_ASN1_C_API SALTS_ASN1_C_API
#endif

#endif /* SALTS_ASN1_API_H */
