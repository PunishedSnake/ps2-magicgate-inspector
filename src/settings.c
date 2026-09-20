#include "settings.h"

#include <fileXio_rpc.h>
#include <iox_stat.h>
#include <io_common.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define SETTINGS_CONFIG_MAX_BYTES 2048u

static const char *ConfigRoots[] = {
    "mass:",
    "mass0:",
    "mass1:"
};

static const char *FsProfileId(MciFsTestProfile profile)
{
    switch (profile) {
        case MCI_FS_TEST_EXTENDED: return "extended";
        case MCI_FS_TEST_THOROUGH: return "thorough";
        case MCI_FS_TEST_QUICK:
        default: return "quick";
    }
}

static const char *VerifyModeId(MciInstallVerifyMode mode)
{
    switch (mode) {
        case MCI_INSTALL_VERIFY_REQUIRED: return "required";
        case MCI_INSTALL_VERIFY_DISABLED: return "disabled";
        case MCI_INSTALL_VERIFY_ENFORCED:
        default: return "enforced";
    }
}

static int ParseVideoMode(const char *value, MciVideoMode *out)
{
    unsigned int i;

    for (i = 0u; i < MCI_VIDEO_MODE_COUNT; i++) {
        if (strcmp(value, MciVideoModeId((MciVideoMode)i)) == 0) {
            *out = (MciVideoMode)i;
            return 0;
        }
    }
    return -1;
}

static int ParseFsProfile(const char *value, MciFsTestProfile *out)
{
    if (strcmp(value, "quick") == 0)
        *out = MCI_FS_TEST_QUICK;
    else if (strcmp(value, "extended") == 0)
        *out = MCI_FS_TEST_EXTENDED;
    else if (strcmp(value, "thorough") == 0)
        *out = MCI_FS_TEST_THOROUGH;
    else
        return -1;
    return 0;
}

static int ParseVerifyMode(const char *value, MciInstallVerifyMode *out)
{
    if (strcmp(value, "enforced") == 0)
        *out = MCI_INSTALL_VERIFY_ENFORCED;
    else if (strcmp(value, "required") == 0)
        *out = MCI_INSTALL_VERIFY_REQUIRED;
    else if (strcmp(value, "disabled") == 0)
        *out = MCI_INSTALL_VERIFY_DISABLED;
    else
        return -1;
    return 0;
}

static void Trim(char *text)
{
    char *start = text;
    char *end;

    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')
        start++;
    if (start != text)
        memmove(text, start, strlen(start) + 1u);

    end = text + strlen(text);
    while (end > text &&
           (end[-1] == ' ' || end[-1] == '\t' ||
            end[-1] == '\r' || end[-1] == '\n'))
        *--end = '\0';
}

static int RootAvailable(const char *root)
{
    int fd = fileXioDopen(root);
    if (fd < 0)
        return fd;
    fileXioDclose(fd);
    return 0;
}

static int ConfigPathForRoot(const char *root, char *path, unsigned int path_size)
{
    int rc;

    rc = snprintf(path, path_size, "%s/MCI/MCINSPECTOR.CFG", root);
    return rc >= 0 && (unsigned int)rc < path_size ? 0 : -5300;
}

static int EnsureConfigDirectory(const char *root)
{
    char directory[MCI_SETTINGS_CONFIG_PATH_MAX];
    iox_stat_t stat;
    int rc;

    rc = snprintf(directory, sizeof(directory), "%s/MCI", root);
    if (rc < 0 || (unsigned int)rc >= sizeof(directory))
        return -5301;

    memset(&stat, 0, sizeof(stat));
    rc = fileXioGetStat(directory, &stat);
    if (rc >= 0)
        return FIO_S_ISDIR(stat.mode) ? 0 : -5302;

    rc = fileXioMkdir(directory, 0777);
    if (rc >= 0)
        return 0;

    memset(&stat, 0, sizeof(stat));
    if (fileXioGetStat(directory, &stat) >= 0 && FIO_S_ISDIR(stat.mode))
        return 0;
    return rc;
}

static int FindExistingConfig(char *path, unsigned int path_size)
{
    unsigned int i;
    iox_stat_t stat;
    char candidate[MCI_SETTINGS_CONFIG_PATH_MAX];

    for (i = 0u; i < sizeof(ConfigRoots) / sizeof(ConfigRoots[0]); i++) {
        if (ConfigPathForRoot(ConfigRoots[i], candidate, sizeof(candidate)) < 0)
            continue;
        memset(&stat, 0, sizeof(stat));
        if (fileXioGetStat(candidate, &stat) >= 0 && FIO_S_ISREG(stat.mode)) {
            snprintf(path, path_size, "%s", candidate);
            return 0;
        }
    }
    return -ENOENT;
}

static int FindWritableConfig(char *path, unsigned int path_size,
                              char *root_out, unsigned int root_out_size)
{
    unsigned int i;
    char candidate[MCI_SETTINGS_CONFIG_PATH_MAX];
    int rc;

    rc = FindExistingConfig(path, path_size);
    if (rc == 0) {
        const char *colon = strchr(path, ':');
        unsigned int length;
        if (colon == NULL)
            return -5303;
        length = (unsigned int)(colon - path) + 1u;
        if (length >= root_out_size)
            return -5303;
        memcpy(root_out, path, length);
        root_out[length] = '\0';
        return 0;
    }

    for (i = 0u; i < sizeof(ConfigRoots) / sizeof(ConfigRoots[0]); i++) {
        if (RootAvailable(ConfigRoots[i]) < 0)
            continue;
        rc = EnsureConfigDirectory(ConfigRoots[i]);
        if (rc < 0)
            continue;
        rc = ConfigPathForRoot(ConfigRoots[i], candidate, sizeof(candidate));
        if (rc < 0)
            continue;
        snprintf(path, path_size, "%s", candidate);
        snprintf(root_out, root_out_size, "%s", ConfigRoots[i]);
        return 0;
    }
    return -ENODEV;
}

static int ReadConfigFile(const char *path, char *buffer, unsigned int capacity)
{
    iox_stat_t stat;
    unsigned int done = 0u;
    int fd;
    int rc;

    memset(&stat, 0, sizeof(stat));
    rc = fileXioGetStat(path, &stat);
    if (rc < 0)
        return rc;
    if (!FIO_S_ISREG(stat.mode) || stat.size <= 0 ||
        (unsigned int)stat.size >= capacity)
        return -5304;

    fd = fileXioOpen(path, FIO_O_RDONLY);
    if (fd < 0)
        return fd;

    while (done < (unsigned int)stat.size) {
        rc = fileXioRead(fd, buffer + done, stat.size - (int)done);
        if (rc <= 0) {
            fileXioClose(fd);
            return rc < 0 ? rc : -5305;
        }
        done += (unsigned int)rc;
    }
    fileXioClose(fd);
    buffer[done] = '\0';
    return (int)done;
}

static int WriteAll(int fd, const char *buffer, unsigned int size)
{
    unsigned int done = 0u;
    int rc;

    while (done < size) {
        rc = fileXioWrite(fd, buffer + done, (int)(size - done));
        if (rc <= 0)
            return rc < 0 ? rc : -5306;
        done += (unsigned int)rc;
    }
    return 0;
}

void MciSettingsDefaults(MciSettings *settings)
{
    if (settings == 0)
        return;
    settings->video_mode = MCI_VIDEO_NATIVE;
    settings->fs_profile = MCI_FS_TEST_QUICK;
    settings->preserve_existing_cnfs = 1;
    settings->install_verify_mode = MCI_INSTALL_VERIFY_ENFORCED;
}

const char *MciFsTestProfileName(MciFsTestProfile profile)
{
    switch (profile) {
        case MCI_FS_TEST_EXTENDED: return "Extended (64 KiB)";
        case MCI_FS_TEST_THOROUGH: return "Thorough (256 KiB)";
        case MCI_FS_TEST_QUICK:
        default: return "Quick (4 KiB)";
    }
}

unsigned int MciFsTestProfileBytes(MciFsTestProfile profile)
{
    switch (profile) {
        case MCI_FS_TEST_EXTENDED: return 64u * 1024u;
        case MCI_FS_TEST_THOROUGH: return 256u * 1024u;
        case MCI_FS_TEST_QUICK:
        default: return 4u * 1024u;
    }
}

const char *MciInstallVerifyModeName(MciInstallVerifyMode mode)
{
    switch (mode) {
        case MCI_INSTALL_VERIFY_REQUIRED: return "REQUIRED ONLY";
        case MCI_INSTALL_VERIFY_DISABLED: return "DISABLED";
        case MCI_INSTALL_VERIFY_ENFORCED:
        default: return "ENFORCED";
    }
}

int MciSettingsLoadFromMass(MciSettings *settings,
                            char *loaded_path, unsigned int loaded_path_size)
{
    char path[MCI_SETTINGS_CONFIG_PATH_MAX];
    char buffer[SETTINGS_CONFIG_MAX_BYTES];
    MciSettings candidate;
    char *line;
    int version = -1;
    int rc;

    if (settings == NULL)
        return -5307;

    fileXioSetBlockMode(FXIO_WAIT);
    rc = FindExistingConfig(path, sizeof(path));
    if (rc < 0)
        return rc;

    rc = ReadConfigFile(path, buffer, sizeof(buffer));
    if (rc < 0)
        return rc;

    candidate = *settings;
    line = strtok(buffer, "\n");
    while (line != NULL) {
        char *equals;
        char *key;
        char *value;

        Trim(line);
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';') {
            line = strtok(NULL, "\n");
            continue;
        }

        equals = strchr(line, '=');
        if (equals == NULL) {
            line = strtok(NULL, "\n");
            continue;
        }
        *equals = '\0';
        key = line;
        value = equals + 1;
        Trim(key);
        Trim(value);

        if (strcmp(key, "version") == 0) {
            if (strcmp(value, "1") != 0)
                return -5308;
            version = 1;
        } else if (strcmp(key, "video_mode") == 0) {
            if (ParseVideoMode(value, &candidate.video_mode) < 0)
                return -5309;
        } else if (strcmp(key, "fs_profile") == 0) {
            if (ParseFsProfile(value, &candidate.fs_profile) < 0)
                return -5310;
        } else if (strcmp(key, "preserve_existing_cnfs") == 0) {
            if (strcmp(value, "1") == 0)
                candidate.preserve_existing_cnfs = 1;
            else if (strcmp(value, "0") == 0)
                candidate.preserve_existing_cnfs = 0;
            else
                return -5311;
        } else if (strcmp(key, "install_verify_mode") == 0) {
            if (ParseVerifyMode(value, &candidate.install_verify_mode) < 0)
                return -5312;
        }

        line = strtok(NULL, "\n");
    }

    if (version != MCI_SETTINGS_CONFIG_VERSION)
        return -5313;

    *settings = candidate;
    if (loaded_path != NULL && loaded_path_size > 0u)
        snprintf(loaded_path, loaded_path_size, "%s", path);
    return 0;
}

int MciSettingsSaveToMass(const MciSettings *settings,
                          char *saved_path, unsigned int saved_path_size)
{
    char path[MCI_SETTINGS_CONFIG_PATH_MAX];
    char temp[MCI_SETTINGS_CONFIG_PATH_MAX + 8];
    char root[16];
    char buffer[512];
    char verify[512];
    int length;
    int fd;
    int rc;

    if (settings == NULL)
        return -5314;

    fileXioSetBlockMode(FXIO_WAIT);
    rc = FindWritableConfig(path, sizeof(path), root, sizeof(root));
    if (rc < 0)
        return rc;
    rc = EnsureConfigDirectory(root);
    if (rc < 0)
        return rc;

    length = snprintf(buffer, sizeof(buffer),
                      "# PS2 Memory Card Inspector settings\n"
                      "version=%d\n"
                      "video_mode=%s\n"
                      "fs_profile=%s\n"
                      "preserve_existing_cnfs=%d\n"
                      "install_verify_mode=%s\n",
                      MCI_SETTINGS_CONFIG_VERSION,
                      MciVideoModeId(settings->video_mode),
                      FsProfileId(settings->fs_profile),
                      settings->preserve_existing_cnfs ? 1 : 0,
                      VerifyModeId(settings->install_verify_mode));
    if (length < 0 || (unsigned int)length >= sizeof(buffer))
        return -5315;

    if (snprintf(temp, sizeof(temp), "%s.tmp", path) >= (int)sizeof(temp))
        return -5316;

    (void)fileXioRemove(temp);
    fd = fileXioOpen(temp, FIO_O_WRONLY | FIO_O_CREAT);
    if (fd < 0)
        return fd;

    rc = WriteAll(fd, buffer, (unsigned int)length);
    if (rc == 0)
        rc = fileXioClose(fd);
    else
        (void)fileXioClose(fd);
    if (rc < 0) {
        (void)fileXioRemove(temp);
        return rc;
    }

    (void)fileXioSync(root, 0);

    memset(verify, 0, sizeof(verify));
    rc = ReadConfigFile(temp, verify, sizeof(verify));
    if (rc != length || memcmp(buffer, verify, (unsigned int)length) != 0) {
        (void)fileXioRemove(temp);
        return rc < 0 ? rc : -5317;
    }

    (void)fileXioRemove(path);
    rc = fileXioRename(temp, path);
    if (rc < 0) {
        (void)fileXioRemove(temp);
        return rc;
    }
    (void)fileXioSync(root, 0);

    if (saved_path != NULL && saved_path_size > 0u)
        snprintf(saved_path, saved_path_size, "%s", path);
    return 0;
}
