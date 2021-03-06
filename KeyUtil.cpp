/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "KeyUtil.h"

#include <iomanip>
#include <sstream>
#include <string>

#include <openssl/sha.h>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <keyutils.h>

#include "KeyStorage.h"
#include "Utils.h"

namespace android {
namespace vold {

bool randomKey(std::string* key) {
    if (ReadRandomBytes(EXT4_AES_256_XTS_KEY_SIZE, *key) != 0) {
        // TODO status_t plays badly with PLOG, fix it.
        LOG(ERROR) << "Random read failed";
        return false;
    }
    return true;
}

// Get raw keyref - used to make keyname and to pass to ioctl
static std::string generateKeyRef(const char* key, int length) {
    SHA512_CTX c;

    SHA512_Init(&c);
    SHA512_Update(&c, key, length);
    unsigned char key_ref1[SHA512_DIGEST_LENGTH];
    SHA512_Final(key_ref1, &c);

    SHA512_Init(&c);
    SHA512_Update(&c, key_ref1, SHA512_DIGEST_LENGTH);
    unsigned char key_ref2[SHA512_DIGEST_LENGTH];
    SHA512_Final(key_ref2, &c);

    static_assert(EXT4_KEY_DESCRIPTOR_SIZE <= SHA512_DIGEST_LENGTH,
                  "Hash too short for descriptor");
    return std::string((char*)key_ref2, EXT4_KEY_DESCRIPTOR_SIZE);
}

static bool fillKey(const std::string& key, ext4_encryption_key* ext4_key) {
    if (key.size() != EXT4_AES_256_XTS_KEY_SIZE) {
        LOG(ERROR) << "Wrong size key " << key.size();
        return false;
    }
    static_assert(EXT4_AES_256_XTS_KEY_SIZE <= sizeof(ext4_key->raw), "Key too long!");
    ext4_key->mode = EXT4_ENCRYPTION_MODE_AES_256_XTS;
    ext4_key->size = key.size();
    memset(ext4_key->raw, 0, sizeof(ext4_key->raw));
    memcpy(ext4_key->raw, key.data(), key.size());
    return true;
}

std::string keyname(const std::string& raw_ref) {
    std::ostringstream o;
    o << "ext4:";
    for (auto i : raw_ref) {
        o << std::hex << std::setw(2) << std::setfill('0') << (int)i;
    }
    return o.str();
}

// Get the keyring we store all keys in
static bool e4cryptKeyring(key_serial_t* device_keyring) {
    *device_keyring = keyctl_search(KEY_SPEC_SESSION_KEYRING, "keyring", "e4crypt", 0);
    if (*device_keyring == -1) {
        PLOG(ERROR) << "Unable to find device keyring";
        return false;
    }
    return true;
}

// Install password into global keyring
// Return raw key reference for use in policy
bool installKey(const std::string& key, std::string* raw_ref) {
    ext4_encryption_key ext4_key;
    if (!fillKey(key, &ext4_key)) return false;
    *raw_ref = generateKeyRef(ext4_key.raw, ext4_key.size);
    auto ref = keyname(*raw_ref);
    key_serial_t device_keyring;
    if (!e4cryptKeyring(&device_keyring)) return false;
    key_serial_t key_id =
        add_key("logon", ref.c_str(), (void*)&ext4_key, sizeof(ext4_key), device_keyring);
    if (key_id == -1) {
        PLOG(ERROR) << "Failed to insert key into keyring " << device_keyring;
        return false;
    }
    LOG(DEBUG) << "Added key " << key_id << " (" << ref << ") to keyring " << device_keyring
               << " in process " << getpid();

    return true;
}

bool evictKey(const std::string& raw_ref) {
    auto ref = keyname(raw_ref);
    key_serial_t device_keyring;
    if (!e4cryptKeyring(&device_keyring)) return false;
    auto key_serial = keyctl_search(device_keyring, "logon", ref.c_str(), 0);

    // Unlink the key from the keyring.  Prefer unlinking to revoking or
    // invalidating, since unlinking is actually no less secure currently, and
    // it avoids bugs in certain kernel versions where the keyring key is
    // referenced from places it shouldn't be.
    if (keyctl_unlink(key_serial, device_keyring) != 0) {
        PLOG(ERROR) << "Failed to unlink key with serial " << key_serial << " ref " << ref;
        return false;
    }
    LOG(DEBUG) << "Unlinked key with serial " << key_serial << " ref " << ref;
    return true;
}

bool retrieveAndInstallKey(bool create_if_absent, const std::string& key_path,
                           const std::string& tmp_path, std::string* key_ref) {
    std::string key;
    if (pathExists(key_path)) {
        LOG(DEBUG) << "Key exists, using: " << key_path;
        if (!retrieveKey(key_path, kEmptyAuthentication, &key)) return false;
    } else {
        if (!create_if_absent) {
           LOG(ERROR) << "No key found in " << key_path;
           return false;
        }
        LOG(INFO) << "Creating new key in " << key_path;
        if (!randomKey(&key)) return false;
        if (!storeKeyAtomically(key_path, tmp_path,
                kEmptyAuthentication, key)) return false;
    }

    if (!installKey(key, key_ref)) {
        LOG(ERROR) << "Failed to install key in " << key_path;
        return false;
    }
    return true;
}

bool retrieveKey(bool create_if_absent, const std::string& key_path,
                 const std::string& tmp_path, std::string* key) {
    if (pathExists(key_path)) {
        LOG(DEBUG) << "Key exists, using: " << key_path;
        if (!retrieveKey(key_path, kEmptyAuthentication, key)) return false;
    } else {
        if (!create_if_absent) {
           LOG(ERROR) << "No key found in " << key_path;
           return false;
        }
        LOG(INFO) << "Creating new key in " << key_path;
        if (!randomKey(key)) return false;
        if (!storeKeyAtomically(key_path, tmp_path,
                kEmptyAuthentication, *key)) return false;
    }
    return true;
}

}  // namespace vold
}  // namespace android
