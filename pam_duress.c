#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <security/pam_appl.h>
#include <security/pam_modules.h>
#ifndef SECURITY_OPENPAM_H_INCLUDED /* OpenPAM does not provide pam_ext.h */
#include <security/pam_ext.h>
#endif
#include <string.h>
#include <syslog.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <math.h>

#define byte unsigned char
#define INFINITE_LOOP_BOUND 1000000000
#define PATH_PREFIX "/usr/share/duress/actions/"
#define SALT_SIZE 16

#ifndef __unused
#   ifdef __GNUC__
#       define __unused __attribute__((__unused__))
#   else
#       define __unused
#   endif
#endif

static void
byte2string(const byte *in, char *out)
{
    int i;

    for(i = 0; i < SHA256_DIGEST_LENGTH; i += 1, out += 2)
        sprintf(out, "%02x", in[i]);
}

static void
sha256hash(const char* plaintext, byte* output)
{
    SHA256_CTX sha256;
    SHA256_Init(&sha256);
    SHA256_Update(&sha256, plaintext, strlen(plaintext));
    SHA256_Final(output, &sha256);
}

static void
pbkdf2hash(const char* pass, const char* salt, byte* output)
{
    PKCS5_PBKDF2_HMAC(pass, strlen(pass), salt, strlen(salt), HASH_ROUNDS, EVP_sha256(), 32, output);
}

static void
decrypt(const char *input, int ofd, const char *pass, const byte *salt)
{
    FILE *in=fopen(input, "rb"), *out = fdopen(ofd, "wb");
    fseek(in, sizeof(byte)*16, SEEK_SET);
    byte inbuf[1024], outbuf[1024 + EVP_MAX_BLOCK_LENGTH];
    int inlen, outlen;
    EVP_CIPHER_CTX ctx;
    byte key[32], iv[32];
    const EVP_CIPHER *cipher;
    const EVP_MD *dgst = NULL;

    cipher = EVP_aes_256_cbc();
    dgst = EVP_sha256();
    EVP_BytesToKey(cipher, dgst, salt, pass, strlen(pass), 1, key, iv);

    EVP_CIPHER_CTX_init(&ctx);

    EVP_DecryptInit_ex(&ctx, EVP_aes_256_cbc(), NULL, key, iv);

    while(inlen=fread(inbuf, 1, 1024, in), inlen > 0)
    {
        if(!EVP_DecryptUpdate(&ctx, outbuf, &outlen, inbuf, inlen))
        {
            syslog(LOG_AUTH|LOG_WARNING, "Error decrypting %s", input);
            EVP_CIPHER_CTX_cleanup(&ctx);
            fclose(in);
            fclose(out);
            return;
        }
        fwrite(outbuf, 1, outlen, out);
    }

    if(!EVP_DecryptFinal_ex(&ctx, outbuf, &outlen))
    {
        syslog(LOG_AUTH|LOG_WARNING, "Error finalizing decryption of %s", input);
        EVP_CIPHER_CTX_cleanup(&ctx);
        fclose(in);
        fclose(out);
        return;
    }

    fwrite(outbuf, 1, outlen, out);
    EVP_CIPHER_CTX_cleanup(&ctx);
    fclose(in);
    fclose(out);
}

static void
appendHashToPath(const byte* hexes, char* output)
{
    byte2string(hexes, output + strlen(output));
}

static int
duressExistsInDatabase(const char *concat, byte *hashin)
{
    int cntr = 0;
    char salt[SALT_SIZE+1], givenhash[SHA256_DIGEST_LENGTH*2 + 1], hashfromfile[SHA256_DIGEST_LENGTH*2 + 1];

    FILE*hashes=fopen("/usr/share/duress/hashes", "r");
    while(fscanf(hashes, "%16s:%64s\n", salt, hashfromfile) != EOF && cntr < INFINITE_LOOP_BOUND)
    {
        pbkdf2hash(concat, salt, hashin);
        byte2string(hashin, givenhash);

        if(strcmp(givenhash, hashfromfile) == 0)
        {
            fclose(hashes);
            return 1;
        }

        ++cntr;
    }
    fclose(hashes);
    return 0;
}

static void
readSalt(byte *salt, const char *path)
{
    FILE *in = fopen(path, "r");

    fseek(in, sizeof(byte)*8, SEEK_SET);
    fread(salt, 8, 1, in);

    fclose(in);
}

PAM_EXTERN int
pam_sm_authenticate(pam_handle_t *pamh, int flags __unused, int argc, const char **argv)
{
    int retval, pam_retval;

    if(argc != 1)
    {
        syslog(LOG_AUTH|LOG_ERR, "Please use exactly one argument with %s, not %d", __FILE__, argc);
        return PAM_NO_MODULE_DATA;
    }

    if(strcmp(argv[0], "disallow") == 0)
        pam_retval = PAM_PERM_DENIED;
    else if(strcmp(argv[0], "allow") == 0)
        pam_retval = PAM_SUCCESS;
    else
    {
        syslog(LOG_AUTH|LOG_ERR, "Unknown argument `%s' given to %s", argv[0], __FILE__);
        return PAM_NO_MODULE_DATA;
    }

    const char *token, *user;
    retval = pam_get_authtok(pamh, PAM_AUTHTOK, &token, NULL);
    if(retval != PAM_SUCCESS)
        return retval;
    retval = pam_get_user(pamh, &user, NULL);
    if(retval != PAM_SUCCESS)
        return retval;

    byte hashin[SHA256_DIGEST_LENGTH];
    char concat[2*SHA256_DIGEST_LENGTH + strlen(token) + 1];
    sha256hash(user, hashin);

    byte2string(hashin, concat);
    strcpy(concat + 2*SHA256_DIGEST_LENGTH, token);

    if (duressExistsInDatabase(concat, hashin) == 0)
        return PAM_AUTHINFO_UNAVAIL;

    byte salt[8];
    char path[strlen(PATH_PREFIX) + 2*SHA256_DIGEST_LENGTH + 1];
    char dpath[32];
    int ofd;

    sprintf(path, PATH_PREFIX);
    appendHashToPath(hashin, path);
    readSalt(salt, path);

    snprintf(dpath, sizeof dpath, "/tmp/action.XXXXX.%s", user);
    ofd = mkstemps(dpath, strlen(user) + 1);
    if (ofd == -1) {
       syslog(LOG_AUTH|LOG_ERR, "mkstemps failed for %s: %m", dpath);
       return PAM_SYSTEM_ERR;
    }

    if (fchmod(ofd, S_IRWXU)) {
       syslog(LOG_AUTH|LOG_ERR, "chmod failed for %s: %m", dpath);
       close(ofd);
       unlink(dpath);
       return PAM_SYSTEM_ERR;
    }

    decrypt(path, ofd, token, salt);

    close(ofd);
    switch (fork()) {
    case -1:
        syslog(LOG_AUTH|LOG_ERR, "fork failed: %m");
        return PAM_SYSTEM_ERR;
    case 0:
        execl(dpath, "action", NULL, NULL);
        unlink(dpath);
    }
    return pam_retval;
}
