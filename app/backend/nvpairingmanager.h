#pragma once

#include "identitymanager.h"
#include "nvhttp.h"
#include "omnuv/pairingnetwork.h"

#include <openssl/x509.h>
#include <openssl/evp.h>

class NvPairingManager
{
public:
    enum PairState
    {
        PAIRED,
        PIN_WRONG,
        FAILED,
        ALREADY_IN_PROGRESS
    };

    explicit NvPairingManager(NvComputer* computer, OmnuvPairingCancellation cancel = {});

    ~NvPairingManager();

    // The certificate reply waits for PIN entry, but must not hold a connection
    // slot forever. Subsequent protocol requests retain their five-second budget.
    static constexpr int CertificateTimeoutMs = 300000;
    PairState
    pair(QString appVersion, QString pin, QSslCertificate& serverCert,
         int certificateTimeoutMs = CertificateTimeoutMs);

private:
    QByteArray
    generateRandomBytes(int length);

    QByteArray
    saltPin(const QByteArray& salt, QString pin);

    QByteArray
    encrypt(const QByteArray& plaintext, const QByteArray& key);

    QByteArray
    decrypt(const QByteArray& ciphertext, const QByteArray& key);

    QByteArray
    getSignatureFromCert(X509* cert);

    QByteArray
    getSignatureFromPemCert(const QByteArray& certificate);

    bool
    verifySignature(const QByteArray& data, const QByteArray& signature, const QByteArray& serverCertificate);

    QByteArray
    signMessage(const QByteArray& message);

    OmnuvPairingNetwork m_Network;
    NvHTTP m_Http;
    X509* m_Cert;
    EVP_PKEY* m_PrivateKey;
};
