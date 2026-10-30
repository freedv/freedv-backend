//=========================================================================
// Name:            TestCertificate.h
// Purpose:         Self-signed TLS certificate for "localhost", generated at
//                  runtime for the unit tests' fake TLS servers.
//
// License:
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License version 2.1,
//  as published by the Free Software Foundation.  This program is
//  distributed in the hope that it will be useful, but WITHOUT ANY
//  WARRANTY; without even the implied warranty of MERCHANTABILITY or
//  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
//  License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, see <http://www.gnu.org/licenses/>.
//
//=========================================================================

#ifndef TEST_CERTIFICATE_H
#define TEST_CERTIFICATE_H

#include <cstdio>
#include <string>

#include "TestSocketCompat.h" // before the OpenSSL headers (see there)

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

// Self-signed certificate for "localhost", generated at runtime so the test
// carries no key material. The client trusts it via SSL_CERT_FILE.
struct TestCertificate
{
    EVP_PKEY* key = nullptr;
    X509* cert = nullptr;
    std::string path;

    bool generate()
    {
        EVP_PKEY_CTX* keyCtx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
        if (keyCtx == nullptr ||
            EVP_PKEY_keygen_init(keyCtx) <= 0 ||
            EVP_PKEY_CTX_set_ec_paramgen_curve_nid(keyCtx, NID_X9_62_prime256v1) <= 0 ||
            EVP_PKEY_keygen(keyCtx, &key) <= 0)
        {
            EVP_PKEY_CTX_free(keyCtx);
            return false;
        }
        EVP_PKEY_CTX_free(keyCtx);

        cert = X509_new();
        X509_set_version(cert, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
        X509_gmtime_adj(X509_getm_notBefore(cert), -3600);
        X509_gmtime_adj(X509_getm_notAfter(cert), 3600);
        X509_set_pubkey(cert, key);

        // Build the name separately: OpenSSL 4's X509_get_subject_name()
        // returns a const name that can't be added to.
        X509_NAME* name = X509_NAME_new();
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char*)"localhost", -1, -1, 0);
        X509_set_subject_name(cert, name);
        X509_set_issuer_name(cert, name);
        X509_NAME_free(name);

        X509V3_CTX extCtx;
        X509V3_set_ctx_nodb(&extCtx);
        X509V3_set_ctx(&extCtx, cert, cert, nullptr, nullptr, 0);
        const char* exts[][2] = {
            {"basicConstraints", "critical,CA:TRUE"},
            {"subjectAltName", "DNS:localhost"},
        };
        for (auto& ext : exts)
        {
            X509_EXTENSION* extension = X509V3_EXT_conf(nullptr, &extCtx, (char*)ext[0], (char*)ext[1]);
            if (extension == nullptr)
            {
                return false;
            }
            X509_add_ext(cert, extension, -1);
            X509_EXTENSION_free(extension);
        }

        if (X509_sign(cert, key, EVP_sha256()) <= 0)
        {
            return false;
        }

        path = testMakeTempFile("fdvcert");
        FILE* fp = path.empty() ? nullptr : fopen(path.c_str(), "w");
        if (fp == nullptr)
        {
            return false;
        }
        PEM_write_X509(fp, cert);
        fclose(fp);
        return true;
    }

    ~TestCertificate()
    {
        if (!path.empty())
        {
            std::remove(path.c_str());
        }
        X509_free(cert);
        EVP_PKEY_free(key);
    }
};

#endif // TEST_CERTIFICATE_H
