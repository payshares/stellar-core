// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SecretKey.h"
#include "crypto/Hex.h"
#include "crypto/KeyUtils.h"
#include "crypto/SHA.h"
#include "crypto/PsrKey.h"
#include "main/Config.h"
#include "transactions/SignatureUtils.h"
#include "util/HashOfHash.h"
#include "util/lrucache.hpp"
#include "util/make_unique.h"
#include <memory>
#include <mutex>
#include <sodium.h>
#include <type_traits>

namespace payshares
{

// Process-wide global Ed25519 signature-verification cache.
//
// This is a pure mathematical function and has no relationship
// to the state of the process; caching its results centrally
// makes all signature-verification in the program faster and
// has no effect on correctness.

static std::mutex gVerifySigCacheMutex;
static cache::lru_cache<Hash, bool> gVerifySigCache(0xffff);
static std::unique_ptr<SHA256> gHasher = SHA256::create();
static uint64_t gVerifyCacheHit = 0;
static uint64_t gVerifyCacheMiss = 0;

static Hash
verifySigCacheKey(PublicKey const& key, Signature const& signature,
                  ByteSlice const& bin)
{
    assert(key.type() == PUBLIC_KEY_TYPE_ED25519);

    gHasher->reset();
    gHasher->add(key.ed25519());
    gHasher->add(signature);
    gHasher->add(bin);
    return gHasher->finish();
}

SecretKey::SecretKey() : mKeyType(PUBLIC_KEY_TYPE_ED25519)
{
    static_assert(crypto_sign_PUBLICKEYBYTES == sizeof(uint256),
                  "Unexpected public key length");
    static_assert(crypto_sign_SEEDBYTES == sizeof(uint256),
                  "Unexpected seed length");
    static_assert(crypto_sign_SECRETKEYBYTES == sizeof(uint512),
                  "Unexpected secret key length");
    static_assert(crypto_sign_BYTES == sizeof(uint512),
                  "Unexpected signature length");
}

SecretKey::~SecretKey()
{
    std::memset(mSecretKey.data(), 0, mSecretKey.size());
}

SecretKey::Seed::~Seed()
{
    std::memset(mSeed.data(), 0, mSeed.size());
}

PublicKey
SecretKey::getPublicKey() const
{
    PublicKey pk;

    assert(mKeyType == PUBLIC_KEY_TYPE_ED25519);

    if (crypto_sign_ed25519_sk_to_pk(pk.ed25519().data(), mSecretKey.data()) !=
        0)
    {
        throw std::runtime_error("error extracting public key from secret key");
    }
    return pk;
}

SecretKey::Seed
SecretKey::getSeed() const
{
    assert(mKeyType == PUBLIC_KEY_TYPE_ED25519);

    Seed seed;
    seed.mKeyType = mKeyType;
    if (crypto_sign_ed25519_sk_to_seed(seed.mSeed.data(), mSecretKey.data()) !=
        0)
    {
        throw std::runtime_error("error extracting seed from secret key");
    }
    return seed;
}

SecretValue
SecretKey::getPsrKeySeed() const
{
    assert(mKeyType == PUBLIC_KEY_TYPE_ED25519);

    return psrKey::toPsrKey(psrKey::PSRKEY_SEED_ED25519, getSeed().mSeed);
}

std::string
SecretKey::getPsrKeyPublic() const
{
    return KeyUtils::toPsrKey(getPublicKey());
}

bool
SecretKey::isZero() const
{
    for (auto i : mSecretKey)
    {
        if (i != 0)
        {
            return false;
        }
    }
    return true;
}

Signature
SecretKey::sign(ByteSlice const& bin) const
{
    assert(mKeyType == PUBLIC_KEY_TYPE_ED25519);

    Signature out(crypto_sign_BYTES, 0);
    if (crypto_sign_detached(out.data(), NULL, bin.data(), bin.size(),
                             mSecretKey.data()) != 0)
    {
        throw std::runtime_error("error while signing");
    }
    return out;
}

SecretKey
SecretKey::random()
{
    PublicKey pk;
    SecretKey sk;
    assert(sk.mKeyType == PUBLIC_KEY_TYPE_ED25519);
    if (crypto_sign_keypair(pk.ed25519().data(), sk.mSecretKey.data()) != 0)
    {
        throw std::runtime_error("error generating random secret key");
    }
    return sk;
}

SecretKey
SecretKey::fromSeed(ByteSlice const& seed)
{
    PublicKey pk;
    SecretKey sk;
    assert(sk.mKeyType == PUBLIC_KEY_TYPE_ED25519);

    if (seed.size() != crypto_sign_SEEDBYTES)
    {
        throw std::runtime_error("seed does not match byte size");
    }
    if (crypto_sign_seed_keypair(pk.ed25519().data(), sk.mSecretKey.data(),
                                 seed.data()) != 0)
    {
        throw std::runtime_error("error generating secret key from seed");
    }
    return sk;
}

SecretKey
SecretKey::fromPsrKeySeed(std::string const& psrKeySeed)
{
    uint8_t ver;
    std::vector<uint8_t> seed;
    if (!psrKey::fromPsrKey(psrKeySeed, ver, seed) ||
        (ver != psrKey::PSRKEY_SEED_ED25519) ||
        (seed.size() != crypto_sign_SEEDBYTES) ||
        (psrKeySeed.size() != psrKey::getPsrKeySize(crypto_sign_SEEDBYTES)))
    {
        throw std::runtime_error("invalid seed");
    }

    PublicKey pk;
    SecretKey sk;
    assert(sk.mKeyType == PUBLIC_KEY_TYPE_ED25519);
    if (crypto_sign_seed_keypair(pk.ed25519().data(), sk.mSecretKey.data(),
                                 seed.data()) != 0)
    {
        throw std::runtime_error("error generating secret key from seed");
    }
    return sk;
}

void
PubKeyUtils::clearVerifySigCache()
{
    std::lock_guard<std::mutex> guard(gVerifySigCacheMutex);
    gVerifySigCache.clear();
}

void
PubKeyUtils::flushVerifySigCacheCounts(uint64_t& hits, uint64_t& misses)
{
    std::lock_guard<std::mutex> guard(gVerifySigCacheMutex);
    hits = gVerifyCacheHit;
    misses = gVerifyCacheMiss;
    gVerifyCacheHit = 0;
    gVerifyCacheMiss = 0;
}

std::string
KeyFunctions<PublicKey>::getKeyTypeName()
{
    return "public key";
}

bool
KeyFunctions<PublicKey>::getKeyVersionIsSupported(
    psrKey::PsrKeyVersionByte keyVersion)
{
    switch (keyVersion)
    {
    case psrKey::PSRKEY_PUBKEY_ED25519:
        return true;
    default:
        return false;
    }
}

PublicKeyType
KeyFunctions<PublicKey>::toKeyType(psrKey::PsrKeyVersionByte keyVersion)
{
    switch (keyVersion)
    {
    case psrKey::PSRKEY_PUBKEY_ED25519:
        return PublicKeyType::PUBLIC_KEY_TYPE_ED25519;
    default:
        throw std::invalid_argument("invalid public key type");
    }
}

psrKey::PsrKeyVersionByte
KeyFunctions<PublicKey>::toKeyVersion(PublicKeyType keyType)
{
    switch (keyType)
    {
    case PublicKeyType::PUBLIC_KEY_TYPE_ED25519:
        return psrKey::PSRKEY_PUBKEY_ED25519;
    default:
        throw std::invalid_argument("invalid public key type");
    }
}

uint256&
KeyFunctions<PublicKey>::getKeyValue(PublicKey& key)
{
    switch (key.type())
    {
    case SIGNER_KEY_TYPE_ED25519:
        return key.ed25519();
    default:
        throw std::invalid_argument("invalid public key type");
    }
}

uint256 const&
KeyFunctions<PublicKey>::getKeyValue(PublicKey const& key)
{
    switch (key.type())
    {
    case SIGNER_KEY_TYPE_ED25519:
        return key.ed25519();
    default:
        throw std::invalid_argument("invalid public key type");
    }
}

bool
PubKeyUtils::verifySig(PublicKey const& key, Signature const& signature,
                       ByteSlice const& bin)
{
    assert(key.type() == PUBLIC_KEY_TYPE_ED25519);
    if (signature.size() != 64)
    {
        return false;
    }

    auto cacheKey = verifySigCacheKey(key, signature, bin);

    {
        std::lock_guard<std::mutex> guard(gVerifySigCacheMutex);
        if (gVerifySigCache.exists(cacheKey))
        {
            ++gVerifyCacheHit;
            return gVerifySigCache.get(cacheKey);
        }
    }

    ++gVerifyCacheMiss;
    bool ok =
        (crypto_sign_verify_detached(signature.data(), bin.data(), bin.size(),
                                     key.ed25519().data()) == 0);
    std::lock_guard<std::mutex> guard(gVerifySigCacheMutex);
    gVerifySigCache.put(cacheKey, ok);
    return ok;
}

PublicKey
PubKeyUtils::random()
{
    PublicKey pk;
    pk.type(PUBLIC_KEY_TYPE_ED25519);
    pk.ed25519().resize(crypto_sign_PUBLICKEYBYTES);
    randombytes_buf(pk.ed25519().data(), pk.ed25519().size());
    return pk;
}

static void
logPublicKey(std::ostream& s, PublicKey const& pk)
{
    s << "PublicKey:" << std::endl
      << "  psrKey: " << KeyUtils::toPsrKey(pk) << std::endl
      << "  hex: " << binToHex(pk.ed25519()) << std::endl;
}

static void
logSecretKey(std::ostream& s, SecretKey const& sk)
{
    s << "Seed:" << std::endl
      << "  psrKey: " << sk.getPsrKeySeed().value << std::endl;
    logPublicKey(s, sk.getPublicKey());
}

void
PsrKeyUtils::logKey(std::ostream& s, std::string const& key)
{
    // if it's a hex string, display it in all forms
    try
    {
        uint256 data = hexToBin256(key);
        PublicKey pk;
        pk.type(PUBLIC_KEY_TYPE_ED25519);
        pk.ed25519() = data;
        logPublicKey(s, pk);

        SecretKey sk(SecretKey::fromSeed(data));
        logSecretKey(s, sk);
        return;
    }
    catch (...)
    {
    }

    // see if it's a public key
    try
    {
        PublicKey pk = KeyUtils::fromPsrKey<PublicKey>(key);
        logPublicKey(s, pk);
        return;
    }
    catch (...)
    {
    }

    // see if it's a seed
    try
    {
        SecretKey sk = SecretKey::fromPsrKeySeed(key);
        logSecretKey(s, sk);
        return;
    }
    catch (...)
    {
    }
    s << "Unknown key type" << std::endl;
}

Hash
HashUtils::random()
{
    Hash res;
    randombytes_buf(res.data(), res.size());
    return res;
}
}

namespace std
{
size_t
hash<payshares::PublicKey>::operator()(payshares::PublicKey const& k) const noexcept
{
    assert(k.type() == payshares::PUBLIC_KEY_TYPE_ED25519);

    return std::hash<payshares::uint256>()(k.ed25519());
}
}
