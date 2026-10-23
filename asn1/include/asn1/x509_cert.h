/**
 * @file x509_cert.h
 * @brief X.509 Certificate Parser (RFC 5280)
 *
 * Parses X.509 v3 certificates in DER format.
 * Used for TLS/DTLS, code signing, and PKI applications.
 */

#ifndef X509_CERT_H
#define X509_CERT_H


#include "asn1_api.h"
#include "platform.h"
#include "asn1_der_compat.h"
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

/* ============================================================================
 * X.509 Data Structures
 * ============================================================================ */

/**
 * X.509 Name (Distinguished Name)
 * Example: CN=example.com, O=Example Corp, C=US
 */
typedef struct {
    char *common_name;        // CN
    char *organization;       // O
    char *organizational_unit; // OU
    char *country;            // C
    char *state;              // ST
    char *locality;           // L
    char *email;              // emailAddress

    // Raw DER encoding (for signature verification)
    uint8_t *der;
    size_t der_len;
} x509_name_t;

/**
 * Algorithm Identifier
 */
typedef struct {
    uint32_t *oid;           // OID components
    size_t oid_count;

    uint8_t *parameters;     // Optional parameters (DER encoded)
    size_t parameters_len;
} x509_algorithm_t;

/**
 * Certificate Validity Period
 */
typedef struct {
    time_t not_before;
    time_t not_after;
} x509_validity_t;

/**
 * Public Key Types
 */
typedef enum {
    X509_PUBKEY_RSA,
    X509_PUBKEY_EC,
    X509_PUBKEY_ED25519,
    X509_PUBKEY_UNKNOWN
} x509_pubkey_type_t;

/**
 * RSA Public Key
 */
typedef struct {
    uint8_t *modulus;        // n
    size_t modulus_len;
    uint8_t *exponent;       // e
    size_t exponent_len;
} x509_rsa_pubkey_t;

/**
 * EC Public Key
 */
typedef struct {
    uint32_t *curve_oid;     // Named curve OID
    size_t curve_oid_count;
    uint8_t *point;          // Uncompressed point
    size_t point_len;
} x509_ec_pubkey_t;

/**
 * Subject Public Key Info
 */
typedef struct {
    x509_algorithm_t algorithm;
    x509_pubkey_type_t key_type;

    union {
        x509_rsa_pubkey_t rsa;
        x509_ec_pubkey_t ec;
        struct {
            uint8_t *data;
            size_t len;
        } raw;
    } key;
} x509_pubkey_info_t;

/**
 * X.509 Certificate Extension
 */
typedef struct {
    uint32_t *oid;
    size_t oid_count;
    bool critical;
    uint8_t *value;
    size_t value_len;
} x509_extension_t;

/**
 * TBS (To Be Signed) Certificate
 */
typedef struct {
    int version;                    // 0=v1, 1=v2, 2=v3

    uint8_t *serial_number;         // Certificate serial number
    size_t serial_number_len;

    x509_algorithm_t signature;     // Signature algorithm
    x509_name_t issuer;             // Certificate issuer
    x509_validity_t validity;       // Validity period
    x509_name_t subject;            // Certificate subject
    x509_pubkey_info_t public_key;  // Subject public key

    // Extensions (v3 only)
    x509_extension_t *extensions;
    size_t extensions_count;

    // Raw DER encoding (for signature verification)
    uint8_t *der;
    size_t der_len;
} x509_tbs_cert_t;

/**
 * X.509 Certificate
 */
typedef struct {
    x509_tbs_cert_t tbs;            // TBSCertificate
    x509_algorithm_t sig_algorithm; // Signature algorithm

    uint8_t *signature;             // Signature value
    size_t signature_len;
    uint8_t signature_unused_bits;  // For BIT STRING

    // Raw DER encoding (original certificate)
    uint8_t *der;
    size_t der_len;
} x509_cert_t;

/* ============================================================================
 * X.509 Certificate Parser
 * ============================================================================ */

/**
 * Parse X.509 certificate from DER bytes
 * @param der DER-encoded certificate
 * @param der_len Certificate length
 * @param cert Output certificate (must free with x509_cert_free)
 * @return 0 on success, -1 on error
 */
SALTSNET_ASN1_C_API int x509_cert_parse(const uint8_t *der, size_t der_len, x509_cert_t **cert);

/**
 * Free X.509 certificate and all resources
 */
SALTSNET_ASN1_C_API void x509_cert_free(x509_cert_t *cert);

/**
 * Free X.509 name
 */
SALTSNET_ASN1_C_API void x509_name_free(x509_name_t *name);

/* ============================================================================
 * Certificate Validation Helpers
 * ============================================================================ */

/**
 * Check if certificate is currently valid (time-wise)
 * @return true if current time is within validity period
 */
SALTSNET_ASN1_C_API bool x509_cert_is_valid_now(const x509_cert_t *cert);

/**
 * Check if certificate is valid at a specific time
 */
SALTSNET_ASN1_C_API bool x509_cert_is_valid_at(const x509_cert_t *cert, time_t t);

/**
 * Get certificate subject common name (CN)
 * @return CN string or NULL if not present
 */
SALTSNET_ASN1_C_API const char *x509_cert_get_subject_cn(const x509_cert_t *cert);

/**
 * Get certificate issuer common name (CN)
 */
SALTSNET_ASN1_C_API const char *x509_cert_get_issuer_cn(const x509_cert_t *cert);

/**
 * Check if certificate is self-signed
 * @return true if issuer == subject
 */
SALTSNET_ASN1_C_API bool x509_cert_is_self_signed(const x509_cert_t *cert);

/**
 * Get public key type
 */
SALTSNET_ASN1_C_API x509_pubkey_type_t x509_cert_get_pubkey_type(const x509_cert_t *cert);

/* ============================================================================
 * OID Constants (Common X.509 OIDs)
 * ============================================================================ */

// Attribute Types (id-at)
#define X509_OID_AT_CN          "2.5.4.3"   // commonName
#define X509_OID_AT_COUNTRY     "2.5.4.6"   // countryName
#define X509_OID_AT_LOCALITY    "2.5.4.7"   // localityName
#define X509_OID_AT_STATE       "2.5.4.8"   // stateOrProvinceName
#define X509_OID_AT_ORG         "2.5.4.10"  // organizationName
#define X509_OID_AT_OU          "2.5.4.11"  // organizationalUnitName
#define X509_OID_AT_EMAIL       "1.2.840.113549.1.9.1"  // emailAddress

// Public Key Algorithms
#define X509_OID_RSA_ENCRYPTION "1.2.840.113549.1.1.1"  // rsaEncryption
#define X509_OID_EC_PUBKEY      "1.2.840.10045.2.1"     // id-ecPublicKey
#define X509_OID_ED25519        "1.3.101.112"           // id-Ed25519

// Signature Algorithms
#define X509_OID_SHA1_RSA       "1.2.840.113549.1.1.5"  // sha1WithRSAEncryption
#define X509_OID_SHA256_RSA     "1.2.840.113549.1.1.11" // sha256WithRSAEncryption
#define X509_OID_SHA384_RSA     "1.2.840.113549.1.1.12" // sha384WithRSAEncryption
#define X509_OID_SHA256_ECDSA   "1.2.840.10045.4.3.2"   // ecdsa-with-SHA256

// EC Curves
#define X509_OID_SECP256R1      "1.2.840.10045.3.1.7"   // prime256v1 (P-256)
#define X509_OID_SECP384R1      "1.3.132.0.34"          // secp384r1 (P-384)

// Extensions (id-ce)
#define X509_OID_EXT_KEY_USAGE         "2.5.29.15"  // keyUsage
#define X509_OID_EXT_SUBJECT_ALT_NAME  "2.5.29.17"  // subjectAltName
#define X509_OID_EXT_BASIC_CONSTRAINTS "2.5.29.19"  // basicConstraints
#define X509_OID_EXT_EXTENDED_KEY_USAGE "2.5.29.37" // extKeyUsage

// Extended Key Usage (id-kp)
#define X509_OID_KP_SERVER_AUTH        "1.3.6.1.5.5.7.3.1" // serverAuth
#define X509_OID_KP_CLIENT_AUTH        "1.3.6.1.5.5.7.3.2" // clientAuth

#endif // X509_CERT_H