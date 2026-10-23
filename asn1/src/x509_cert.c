/**
 * @file x509_cert.c
 * @brief X.509 Certificate Parser Implementation
 */

#include "x509_cert.h"
#include "asn1/asn1_der_compat.h"
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

static bool oid_equals(const uint32_t *oid1, size_t count1, const char *oid_str) {
    uint32_t oid2[16];
    size_t count2 = 0;
    const char *p = oid_str;

    while (*p && count2 < 16) {
        char *end;
        unsigned long val = strtoul(p, &end, 10);
        if (p == end) break;
        oid2[count2++] = (uint32_t)val;
        p = (*end == '.') ? end + 1 : end;
    }

    if (count1 != count2) return false;
    for (size_t i = 0; i < count1; i++) {
        if (oid1[i] != oid2[i]) return false;
    }
    return true;
}

static char *string_from_asn1(const asn1_value_t *v) {
    if (!v || v->value.octet_string.length == 0) return NULL;

    char *str = malloc(v->value.octet_string.length + 1);
    if (!str) return NULL;

    memcpy(str, v->value.octet_string.data, v->value.octet_string.length);
    str[v->value.octet_string.length] = '\0';
    return str;
}

static time_t parse_asn1_time(const asn1_value_t *v) {
    if (!v || v->value.octet_string.length == 0) return 0;

    struct tm tm = {0};
    const char *str = (const char *)v->value.octet_string.data;
    size_t len = v->value.octet_string.length;

    if (v->type == ASN1_TYPE_UTC_TIME) {
        // YYMMDDhhmmssZ (13 chars)
        if (len < 13) return 0;
        int year = (str[0] - '0') * 10 + (str[1] - '0');
        tm.tm_year = (year < 50 ? 2000 + year : 1900 + year) - 1900;
        tm.tm_mon = (str[2] - '0') * 10 + (str[3] - '0') - 1;
        tm.tm_mday = (str[4] - '0') * 10 + (str[5] - '0');
        tm.tm_hour = (str[6] - '0') * 10 + (str[7] - '0');
        tm.tm_min = (str[8] - '0') * 10 + (str[9] - '0');
        tm.tm_sec = (str[10] - '0') * 10 + (str[11] - '0');
    } else if (v->type == ASN1_TYPE_GENERALIZED_TIME) {
        // YYYYMMDDhhmmssZ (15 chars)
        if (len < 15) return 0;
        tm.tm_year = ((str[0] - '0') * 1000 + (str[1] - '0') * 100 +
                      (str[2] - '0') * 10 + (str[3] - '0')) - 1900;
        tm.tm_mon = (str[4] - '0') * 10 + (str[5] - '0') - 1;
        tm.tm_mday = (str[6] - '0') * 10 + (str[7] - '0');
        tm.tm_hour = (str[8] - '0') * 10 + (str[9] - '0');
        tm.tm_min = (str[10] - '0') * 10 + (str[11] - '0');
        tm.tm_sec = (str[12] - '0') * 10 + (str[13] - '0');
    } else {
        return 0;
    }

    return mktime(&tm);
}

/* ============================================================================
 * Name Parsing
 * ============================================================================ */

static int parse_name(asn1_value_t *name_seq, x509_name_t *name) {
    if (!name_seq || name_seq->type != ASN1_TYPE_SEQUENCE) return -1;

    memset(name, 0, sizeof(x509_name_t));

    // Name is SEQUENCE OF RelativeDistinguishedName
    // RelativeDistinguishedName is SET OF AttributeTypeAndValue
    // AttributeTypeAndValue is SEQUENCE { type OID, value ANY }

    for (size_t i = 0; i < name_seq->value.sequence.count; i++) {
        asn1_value_t *rdn = name_seq->value.sequence.children[i];
        if (rdn->type != ASN1_TYPE_SET) continue;

        for (size_t j = 0; j < rdn->value.set.count; j++) {
            asn1_value_t *attr = rdn->value.set.children[j];
            if (attr->type != ASN1_TYPE_SEQUENCE) continue;
            if (attr->value.sequence.count < 2) continue;

            asn1_value_t *oid = attr->value.sequence.children[0];
            asn1_value_t *value = attr->value.sequence.children[1];

            if (oid->type != ASN1_TYPE_OBJECT_IDENTIFIER) continue;

            char *str = string_from_asn1(value);
            if (!str) continue;

            if (oid_equals(oid->value.oid.components, oid->value.oid.count, X509_OID_AT_CN)) {
                free(name->common_name);
                name->common_name = str;
            } else if (oid_equals(oid->value.oid.components, oid->value.oid.count, X509_OID_AT_ORG)) {
                free(name->organization);
                name->organization = str;
            } else if (oid_equals(oid->value.oid.components, oid->value.oid.count, X509_OID_AT_OU)) {
                free(name->organizational_unit);
                name->organizational_unit = str;
            } else if (oid_equals(oid->value.oid.components, oid->value.oid.count, X509_OID_AT_COUNTRY)) {
                free(name->country);
                name->country = str;
            } else if (oid_equals(oid->value.oid.components, oid->value.oid.count, X509_OID_AT_STATE)) {
                free(name->state);
                name->state = str;
            } else if (oid_equals(oid->value.oid.components, oid->value.oid.count, X509_OID_AT_LOCALITY)) {
                free(name->locality);
                name->locality = str;
            } else if (oid_equals(oid->value.oid.components, oid->value.oid.count, X509_OID_AT_EMAIL)) {
                free(name->email);
                name->email = str;
            } else {
                free(str);
            }
        }
    }

    return 0;
}

/* ============================================================================
 * Algorithm Parsing
 * ============================================================================ */

static int parse_algorithm(asn1_value_t *alg_seq, x509_algorithm_t *alg) {
    if (!alg_seq || alg_seq->type != ASN1_TYPE_SEQUENCE) return -1;
    if (alg_seq->value.sequence.count < 1) return -1;

    memset(alg, 0, sizeof(x509_algorithm_t));

    asn1_value_t *oid = alg_seq->value.sequence.children[0];
    if (oid->type != ASN1_TYPE_OBJECT_IDENTIFIER) return -1;

    alg->oid = malloc(oid->value.oid.count * sizeof(uint32_t));
    if (!alg->oid) return -1;

    memcpy(alg->oid, oid->value.oid.components, oid->value.oid.count * sizeof(uint32_t));
    alg->oid_count = oid->value.oid.count;

    // Optional parameters
    if (alg_seq->value.sequence.count > 1) {
        asn1_value_t *params = alg_seq->value.sequence.children[1];
        if (params->type != ASN1_TYPE_NULL) {
            // Store raw DER encoding of parameters
            // (Would need to re-encode, but we'll skip for now)
        }
    }

    return 0;
}

/* ============================================================================
 * Public Key Parsing
 * ============================================================================ */

static int parse_rsa_pubkey(const uint8_t *der, size_t der_len, x509_rsa_pubkey_t *rsa) {
    asn1_value_t *seq = NULL;
    int ret = asn1_der_decode_legacy(der, der_len, &seq);
    if (ret < 0 || !seq) return -1;

    if (seq->type != ASN1_TYPE_SEQUENCE || seq->value.sequence.count < 2) {
        asn1_free(seq);
        return -1;
    }

    // SEQUENCE { modulus INTEGER, exponent INTEGER }
    asn1_value_t *n = seq->value.sequence.children[0];
    asn1_value_t *e = seq->value.sequence.children[1];

    if (n->type != ASN1_TYPE_INTEGER || e->type != ASN1_TYPE_INTEGER) {
        asn1_free(seq);
        return -1;
    }

    // For now, store as raw bytes (would need INTEGER to bytes conversion)
    // This is simplified - real implementation would extract the integer bytes

    asn1_free(seq);
    return 0;
}

static int parse_pubkey_info(asn1_value_t *pubkey_seq, x509_pubkey_info_t *pubkey) {
    if (!pubkey_seq || pubkey_seq->type != ASN1_TYPE_SEQUENCE) return -1;
    if (pubkey_seq->value.sequence.count < 2) return -1;

    memset(pubkey, 0, sizeof(x509_pubkey_info_t));

    // SubjectPublicKeyInfo ::= SEQUENCE {
    //   algorithm AlgorithmIdentifier,
    //   subjectPublicKey BIT STRING }

    asn1_value_t *alg_seq = pubkey_seq->value.sequence.children[0];
    asn1_value_t *pubkey_bits = pubkey_seq->value.sequence.children[1];

    if (parse_algorithm(alg_seq, &pubkey->algorithm) < 0) return -1;
    if (pubkey_bits->type != ASN1_TYPE_BIT_STRING) return -1;

    // Determine key type by OID
    if (oid_equals(pubkey->algorithm.oid, pubkey->algorithm.oid_count, X509_OID_RSA_ENCRYPTION)) {
        pubkey->key_type = X509_PUBKEY_RSA;
        // Parse RSA key from BIT STRING
        parse_rsa_pubkey(pubkey_bits->value.octet_string.data, pubkey_bits->value.octet_string.length, &pubkey->key.rsa);
    } else if (oid_equals(pubkey->algorithm.oid, pubkey->algorithm.oid_count, X509_OID_EC_PUBKEY)) {
        pubkey->key_type = X509_PUBKEY_EC;
        // EC key is just the point
        pubkey->key.ec.point = malloc(pubkey_bits->value.octet_string.length);
        if (pubkey->key.ec.point) {
            memcpy(pubkey->key.ec.point, pubkey_bits->value.octet_string.data, pubkey_bits->value.octet_string.length);
            pubkey->key.ec.point_len = pubkey_bits->value.octet_string.length;
        }
    } else if (oid_equals(pubkey->algorithm.oid, pubkey->algorithm.oid_count, X509_OID_ED25519)) {
        pubkey->key_type = X509_PUBKEY_ED25519;
        // Ed25519 public key is raw 32 bytes
        pubkey->key.raw.data = malloc(pubkey_bits->value.octet_string.length);
        if (pubkey->key.raw.data) {
            memcpy(pubkey->key.raw.data, pubkey_bits->value.octet_string.data, pubkey_bits->value.octet_string.length);
            pubkey->key.raw.len = pubkey_bits->value.octet_string.length;
        }
    } else {
        pubkey->key_type = X509_PUBKEY_UNKNOWN;
        pubkey->key.raw.data = malloc(pubkey_bits->value.octet_string.length);
        if (pubkey->key.raw.data) {
            memcpy(pubkey->key.raw.data, pubkey_bits->value.octet_string.data, pubkey_bits->value.octet_string.length);
            pubkey->key.raw.len = pubkey_bits->value.octet_string.length;
        }
    }

    return 0;
}

/* ============================================================================
 * TBSCertificate Parsing
 * ============================================================================ */

static int parse_tbs_certificate(asn1_value_t *tbs, x509_tbs_cert_t *tbs_cert) {
    if (!tbs || tbs->type != ASN1_TYPE_SEQUENCE) return -1;

    memset(tbs_cert, 0, sizeof(x509_tbs_cert_t));

    size_t idx = 0;
    size_t count = tbs->value.sequence.count;

    // Version [0] EXPLICIT (optional, default v1)
    asn1_value_t *field = tbs->value.sequence.children[idx];
    if (field->tag_class == 2 && field->tag_number == 0) {
        // Context-specific [0]
        if (field->value.sequence.count > 0) {
            asn1_value_t *ver = field->value.sequence.children[0];
            if (ver->type == ASN1_TYPE_INTEGER) {
                tbs_cert->version = (int)ver->value.integer;
            }
        }
        idx++;
    }

    // Serial Number
    if (idx >= count) return -1;
    field = tbs->value.sequence.children[idx++];
    if (field->type == ASN1_TYPE_INTEGER) {
        // Small integer - convert to bytes
        tbs_cert->serial_number_len = 1;
        tbs_cert->serial_number = malloc(1);
        if (tbs_cert->serial_number) {
            tbs_cert->serial_number[0] = (uint8_t)field->value.integer;
        }
    } else {
        // Large integer or other type - copy as octet string
        if (field->type == ASN1_TYPE_OCTET_STRING) {
            tbs_cert->serial_number_len = field->value.octet_string.length;
            tbs_cert->serial_number = malloc(field->value.octet_string.length);
            if (tbs_cert->serial_number) {
                memcpy(tbs_cert->serial_number, field->value.octet_string.data, field->value.octet_string.length);
            }
        } else {
            // Fallback for other types
            tbs_cert->serial_number_len = 0;
            tbs_cert->serial_number = NULL;
        }
    }

    // Signature Algorithm
    if (idx >= count) return -1;
    field = tbs->value.sequence.children[idx++];
    if (parse_algorithm(field, &tbs_cert->signature) < 0) return -1;

    // Issuer
    if (idx >= count) return -1;
    field = tbs->value.sequence.children[idx++];
    if (parse_name(field, &tbs_cert->issuer) < 0) return -1;

    // Validity
    if (idx >= count) return -1;
    field = tbs->value.sequence.children[idx++];
    if (field->type == ASN1_TYPE_SEQUENCE && field->value.sequence.count == 2) {
        tbs_cert->validity.not_before = parse_asn1_time(field->value.sequence.children[0]);
        tbs_cert->validity.not_after = parse_asn1_time(field->value.sequence.children[1]);
    }

    // Subject
    if (idx >= count) return -1;
    field = tbs->value.sequence.children[idx++];
    if (parse_name(field, &tbs_cert->subject) < 0) return -1;

    // SubjectPublicKeyInfo
    if (idx >= count) return -1;
    field = tbs->value.sequence.children[idx++];
    if (parse_pubkey_info(field, &tbs_cert->public_key) < 0) return -1;

    // Extensions [3] EXPLICIT (v3 only)
    while (idx < count) {
        field = tbs->value.sequence.children[idx++];
        // Skip extensions for now
    }

    return 0;
}

/* ============================================================================
 * Certificate Parsing
 * ============================================================================ */

int x509_cert_parse(const uint8_t *der, size_t der_len, x509_cert_t **cert_out) {
    if (!der || der_len == 0 || !cert_out) return -1;

    asn1_value_t *cert_seq = NULL;
    int ret = asn1_der_decode_legacy(der, der_len, &cert_seq);
    if (ret < 0 || !cert_seq) return -1;

    if (cert_seq->type != ASN1_TYPE_SEQUENCE || cert_seq->value.sequence.count < 3) {
        asn1_free(cert_seq);
        return -1;
    }

    x509_cert_t *cert = calloc(1, sizeof(x509_cert_t));
    if (!cert) {
        asn1_free(cert_seq);
        return -1;
    }

    // Certificate ::= SEQUENCE {
    //   tbsCertificate TBSCertificate,
    //   signatureAlgorithm AlgorithmIdentifier,
    //   signatureValue BIT STRING }

    asn1_value_t *tbs = cert_seq->value.sequence.children[0];
    asn1_value_t *sig_alg = cert_seq->value.sequence.children[1];
    asn1_value_t *sig_val = cert_seq->value.sequence.children[2];

    // Parse TBSCertificate
    if (parse_tbs_certificate(tbs, &cert->tbs) < 0) {
        x509_cert_free(cert);
        asn1_free(cert_seq);
        return -1;
    }

    // Parse signature algorithm
    if (parse_algorithm(sig_alg, &cert->sig_algorithm) < 0) {
        x509_cert_free(cert);
        asn1_free(cert_seq);
        return -1;
    }

    // Parse signature value
    if (sig_val->type == ASN1_TYPE_BIT_STRING) {
        cert->signature = malloc(sig_val->value.octet_string.length);
        if (cert->signature) {
            memcpy(cert->signature, sig_val->value.octet_string.data, sig_val->value.octet_string.length);
            cert->signature_len = sig_val->value.octet_string.length;
            // Note: BIT STRING unused bits info is lost in our simplified structure
            cert->signature_unused_bits = 0;
        }
    }

    // Store original DER
    cert->der = malloc(der_len);
    if (cert->der) {
        memcpy(cert->der, der, der_len);
        cert->der_len = der_len;
    }

    asn1_free(cert_seq);
    *cert_out = cert;
    return 0;
}

/* ============================================================================
 * Memory Management
 * ============================================================================ */

void x509_name_free(x509_name_t *name) {
    if (!name) return;
    free(name->common_name);
    free(name->organization);
    free(name->organizational_unit);
    free(name->country);
    free(name->state);
    free(name->locality);
    free(name->email);
    free(name->der);
}

void x509_cert_free(x509_cert_t *cert) {
    if (!cert) return;

    free(cert->tbs.serial_number);
    free(cert->tbs.signature.oid);
    x509_name_free(&cert->tbs.issuer);
    x509_name_free(&cert->tbs.subject);

    free(cert->tbs.public_key.algorithm.oid);
    if (cert->tbs.public_key.key_type == X509_PUBKEY_RSA) {
        free(cert->tbs.public_key.key.rsa.modulus);
        free(cert->tbs.public_key.key.rsa.exponent);
    } else if (cert->tbs.public_key.key_type == X509_PUBKEY_EC) {
        free(cert->tbs.public_key.key.ec.curve_oid);
        free(cert->tbs.public_key.key.ec.point);
    } else {
        free(cert->tbs.public_key.key.raw.data);
    }

    for (size_t i = 0; i < cert->tbs.extensions_count; i++) {
        free(cert->tbs.extensions[i].oid);
        free(cert->tbs.extensions[i].value);
    }
    free(cert->tbs.extensions);
    free(cert->tbs.der);

    free(cert->sig_algorithm.oid);
    free(cert->signature);
    free(cert->der);

    free(cert);
}

/* ============================================================================
 * Validation Helpers
 * ============================================================================ */

bool x509_cert_is_valid_at(const x509_cert_t *cert, time_t t) {
    if (!cert) return false;
    return (t >= cert->tbs.validity.not_before && t <= cert->tbs.validity.not_after);
}

bool x509_cert_is_valid_now(const x509_cert_t *cert) {
    return x509_cert_is_valid_at(cert, time(NULL));
}

const char *x509_cert_get_subject_cn(const x509_cert_t *cert) {
    if (!cert) return NULL;
    return cert->tbs.subject.common_name;
}

const char *x509_cert_get_issuer_cn(const x509_cert_t *cert) {
    if (!cert) return NULL;
    return cert->tbs.issuer.common_name;
}

bool x509_cert_is_self_signed(const x509_cert_t *cert) {
    if (!cert) return false;

    const x509_name_t *issuer = &cert->tbs.issuer;
    const x509_name_t *subject = &cert->tbs.subject;

    #define STR_EQ(a, b) (((a) == (b)) || ((a) && (b) && strcmp((a), (b)) == 0))

    return STR_EQ(issuer->common_name, subject->common_name) &&
           STR_EQ(issuer->organization, subject->organization) &&
           STR_EQ(issuer->country, subject->country);

    #undef STR_EQ
}

x509_pubkey_type_t x509_cert_get_pubkey_type(const x509_cert_t *cert) {
    if (!cert) return X509_PUBKEY_UNKNOWN;
    return cert->tbs.public_key.key_type;
}
