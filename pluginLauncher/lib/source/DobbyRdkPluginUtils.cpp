/*
* If not stated otherwise in this file or this component's LICENSE file the
* following copyright and licenses apply:
*
* Copyright 2020 Sky UK
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#include "DobbyRdkPluginUtils.h"

#include <Logging.h>
#include <string.h>
#include <fstream>
#include <unistd.h>
#include <fcntl.h>
#include <thread>
#include <sstream>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <fcntl.h>
#include <unistd.h>
#include <sstream>
#include <fstream>
#include <iostream>
#include <map>

DobbyRdkPluginUtils::DobbyRdkPluginUtils(const std::shared_ptr<rt_dobby_schema> &cfg)
    : mConf(cfg)
{
    AI_LOG_FN_ENTRY();

    AI_LOG_FN_EXIT();
}

DobbyRdkPluginUtils::DobbyRdkPluginUtils(const std::shared_ptr<rt_dobby_schema> &cfg,
                                         const std::shared_ptr<const rt_state_schema> &state)
    : mConf(cfg)
    , mState(state)
{
    AI_LOG_FN_ENTRY();

    AI_LOG_FN_EXIT();
}

DobbyRdkPluginUtils::~DobbyRdkPluginUtils()
{
    AI_LOG_FN_ENTRY();

    AI_LOG_FN_EXIT();
}

// -------------------------------------------------------------------------
/**
 *  @brief Gets the container pid from the stdin string of a hook.
 *
 *  The stdin needs to be read from within the context of the hook. This
 *  function only parses the pid from a string.
 *
 *  \warning Only returns a valid PID once the container is running. Only works
 *  for OCI hooks.
 *
 *  @return container pid, 0 if none found.
 */
pid_t DobbyRdkPluginUtils::getContainerPid() const
{
    // Must be running a non-OCI hook point
    if (!mState)
    {
        AI_LOG_ERROR_EXIT("Unknown container state - couldn't get pid. Are you running in a non-OCI hook?");
        return 0;
    }

    if (!mState->pid_present)
    {
        AI_LOG_ERROR_EXIT("PID not available");
        return 0;
    }
    return static_cast<pid_t>(mState->pid);
}

// -------------------------------------------------------------------------
/**
 *  @brief Gets the container ID
 *
 *  Since Dobby sets the container hostname to match the container ID, we can
 *  use the hostname. Ideally we'd use the state from stdin, but that's only
 *  available during OCI hooks.
 *
 *  @return Container ID as string
 */
std::string DobbyRdkPluginUtils::getContainerId() const
{
    if (!mConf)
    {
        AI_LOG_ERROR_EXIT("Failed to load config");
        return "";
    }

    return std::string(mConf->hostname);
}

// -------------------------------------------------------------------------
/**
 *  @brief Gets network info about the container (veth/IP)
 *
 * Designed to allow other plugins to create their own iptables rules once the
 * networking plugin has run.
 *
 * @params[out] networkInfo     struct containing veth/ip address
 *
 * @returns true if successfully got info, false for failure
 */
bool DobbyRdkPluginUtils::getContainerNetworkInfo(ContainerNetworkInfo &networkInfo)
{
    // Attempt to find the file
    const std::string containerId = getContainerId();
    if (containerId.empty())
    {
        AI_LOG_ERROR_EXIT("Could not get container network info - could not get ID");
        return false;
    }

    const std::string fileName = ADDRESS_FILE_PREFIX + getContainerId();

    struct stat buffer;
    if (stat(fileName.c_str(), &buffer) != 0)
    {
        AI_LOG_WARN("Could not get container network info - file %s does not exist. Has the network plugin run yet?", fileName.c_str());
        return false;
    }

    // Parse the file
    const std::string addressFileStr = readTextFile(fileName);
    if (addressFileStr.empty())
    {
        AI_LOG_ERROR_EXIT("failed to get IP address and veth name assigned to"
                          "container from %s",
                          fileName.c_str());
        return false;
    }

    const std::string ip = addressFileStr.substr(0, addressFileStr.find("/"));

    // check if string contains a veth name after the ip address
    if (addressFileStr.length() <= ip.length() + 1)
    {
        AI_LOG_ERROR("failed to get veth name from %s", fileName.c_str());
        return false;
    }

    networkInfo.ipAddress = ip;
    networkInfo.vethName = addressFileStr.substr(ip.length() + 1, addressFileStr.length());
    return true;
}

// -----------------------------------------------------------------------------
/**
 *  @brief Thread helper function that implements the setns syscall
 *
 *  This must be executed as a thread as it calls setns which switches
 *  namespaces and you don't really want to do this in the main thread.
 *
 *
 *  @param[in]  newNsFd     The file descriptor for the namespace to
 *                          switch into.
 *  @param[in]  nsType      The type of namespace to switch into i.e.
 *                          "CLONE_NEWIPC".
 *  @param[out] success     Return if thread execution of the function
 *                          was the success.
 *  @param[in]  func        The function to execute in the new namespace.
 */
void DobbyRdkPluginUtils::nsThread(int newNsFd, int nsType, bool* success,
                                   std::function<bool()>& func) const
{
    AI_LOG_FN_ENTRY();

    // unshare the specific namespace from the thread
    if (unshare(nsType) != 0)
    {
        AI_LOG_SYS_ERROR_EXIT(errno, "failed to unshare");
        *success = false;
        return;
    }

    // switch into the new namespace
    if (setns(newNsFd, nsType) != 0)
    {
        AI_LOG_SYS_ERROR_EXIT(errno, "failed to switch into new namespace");
        *success = false;
        return;
    }

    // execute the caller's function
    *success = func();

    AI_LOG_FN_EXIT();
}

// -----------------------------------------------------------------------------
/**
 *  @brief Utility function to run some code in a specific namespace of the
 *  container.
 *
 *  This function uses the setns syscall and therefore it must spawn a thread
 *  to run the callback in.  However this function blocks until the thread
 *  completes, so although it is multi-threaded it's API is blocking, i.e.
 *  effectively single threaded.
 *
 *  The @a nsType argument should be one of the following values:
 *        CLONE_NEWIPC  - run in a IPC namespace
 *        CLONE_NEWNET  - run in a network namespace
 *        CLONE_NEWNS   - run in a mount namespace
 *        CLONE_NEWPID  - run in a PID namespace
 *        CLONE_NEWUSER - run in a user namespace
 *        CLONE_NEWUTS  - run in a UTS namespace
 *
 *  @param[in]  pid         The pid of the process namespace to enter.
 *  @param[in]  nsType      The namespace to run the function in, see above.
 *  @param[in]  func        The function to execute.
 *
 *  @return true if successifully entered the namespace, otherwise false.
 */
bool DobbyRdkPluginUtils::callInNamespaceImpl(pid_t pid, int nsType,
                                     const std::function<bool()>& func) const
{
    AI_LOG_FN_ENTRY();

    char nsName[8];
    char nsPath[32];

    // determine the type of namespace to enter
    switch (nsType)
    {
        case CLONE_NEWIPC:
            strcpy(nsName, "ipc");
            break;
        case CLONE_NEWNET:
            strcpy(nsName, "net");
            break;
        case CLONE_NEWNS:
            strcpy(nsName, "mnt");
            break;
        // the following namespaces are tricky and have special restrictions,
        // at the moment no hook should be using them so disable until needed
        /*
        case CLONE_NEWPID:
            strcpy(nsName, "pid");
            break;
        case CLONE_NEWUSER:
            strcpy(nsName, "user");
            break;
        case CLONE_NEWUTS:
            strcpy(nsName, "uts");
            break;
        */
        case CLONE_NEWPID:
        case CLONE_NEWUSER:
        case CLONE_NEWUTS:
            AI_LOG_ERROR_EXIT("unsupported nsType (%d)", nsType);
            return false;
        default:
            AI_LOG_ERROR_EXIT("invalid nsType (%d)", nsType);
            return false;
    }

    bool success;

    // get the namespace of the containered app
    sprintf(nsPath, "/proc/%d/ns/%s", pid, nsName);
    int newNsFd = open(nsPath, O_RDONLY | O_CLOEXEC);
    if (newNsFd < 0)
    {
        AI_LOG_SYS_ERROR(errno, "failed to open container namespace @ '%s'",
                         nsPath);
        success = false;
    }
    else
    {
        AI_LOG_INFO("about to change namespace to '%s'", nsPath);

        // spawn the thread to run the callback in
        std::thread _nsThread = std::thread(std::bind(&DobbyRdkPluginUtils::nsThread,
                                                      this, newNsFd, nsType,
                                                      &success, func));

        // block until the thread completes
        _nsThread.join();
    }

    // close the namespaces
    if ((newNsFd >= 0) && (close(newNsFd) != 0))
    {
        AI_LOG_SYS_ERROR(errno, "failed to close namespace");
    }

    AI_LOG_FN_EXIT();
    return success;
}

// -------------------------------------------------------------------------
/**
 *  @brief Simply writes a string into a file
 *
 *  @param[in]  path            The path to file to write to.
 *  @param[in]  str             String to write to the file.
 *  @param[in]  flags           Open flags, these will be OR'd with O_WRONLY
 *                              and O_CLOEXEC.
 *  @param[in]  mode            The file access mode to set if O_CREAT was
 *                              specified in flags and the file was created.
 *
 *  @return true on success, false on failure.
 */
bool DobbyRdkPluginUtils::writeTextFile(const std::string &path,
                                        const std::string &str,
                                        int flags,
                                        mode_t mode) const
{
    std::lock_guard<std::mutex> locker(mLock);

    int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC | flags, mode);
    if (fd < 0)
    {
        AI_LOG_SYS_ERROR(errno, "failed to open%s '%s'",
                         (flags & O_CREAT) ? "/create" : "", path.c_str());
        return false;
    }

    const char* size = str.c_str();
    ssize_t remaining = static_cast<ssize_t>(str.length());

    while (remaining > 0)
    {
        ssize_t written = TEMP_FAILURE_RETRY(write(fd, size, remaining));
        if (written < 0)
        {
            AI_LOG_SYS_ERROR(errno, "failed to write to file");
            break;
        }
        else if (written == 0)
        {
            break;
        }

        size += written;
        remaining -= written;
    }

    if (close(fd) != 0)
    {
        AI_LOG_SYS_ERROR(errno, "failed to close '%s'", path.c_str());
    }

    return (remaining == 0);
}

// -------------------------------------------------------------------------
/**
 *  @brief Simply reads a file into a string
 *
 *  @param[in]  path            The path to file to read from.
 *
 *  @return string read from file
 */
std::string DobbyRdkPluginUtils::readTextFile(const std::string &path) const
{
    std::lock_guard<std::mutex> locker(mLock);

    std::ifstream in;
    in.open(path);

    if (!in)
    {
        AI_LOG_SYS_ERROR(errno, "failed to read file '%s'", path.c_str());
        return std::string();
    }

    std::stringstream buffer;
    buffer << in.rdbuf();

    return buffer.str();
}

// -----------------------------------------------------------------------------
/**
 *  @brief Public api to allow for adding additional mounts to a container's
 *  config file.
 *
 *  This can only obviously be called before the config file is persisted to
 *  disk.
 *
 *  @param[in]  source          The mount source
 *  @param[in]  destination     The mount destination
 *  @param[in]  type            The file system type of the mount
 *  @param[in]  mountOptions    The mount options (mount(2) data parameter)
 *
 *  @return true if the mount point was added, otherwise false.
 */
bool DobbyRdkPluginUtils::addMount(const std::string &source,
                                   const std::string &destination,
                                   const std::string &type,
                                   const std::list<std::string> &mountOptions) const
{
    std::lock_guard<std::mutex> locker(mLock);

    AI_LOG_FN_ENTRY();

    // allocate memory for mount
    rt_defs_mount *newMount = (rt_defs_mount*)calloc(1, sizeof(rt_defs_mount));
    newMount->options_len = mountOptions.size();
    newMount->options = (char**)calloc(newMount->options_len, sizeof(char*));

    // add mount options to bundle config
    int i = 0;
    for (const std::string &mountOption : mountOptions)
    {
        newMount->options[i] = strdup(mountOption.c_str());
        i++;
    }

    newMount->destination = strdup(destination.c_str());
    newMount->type = strdup(type.c_str());
    newMount->source = strdup(source.c_str());

    // allocate memory for new mount and place it in the config struct
    mConf->mounts_len++;
    mConf->mounts = (rt_defs_mount**)realloc(mConf->mounts, sizeof(rt_defs_mount *) * mConf->mounts_len);
    mConf->mounts[mConf->mounts_len-1] = newMount;

    AI_LOG_FN_EXIT();
    return true;
}

// -------------------------------------------------------------------------
/**
 *  @brief Makes a directory and all parent directories as needed
 *
 *  This is equivalent to the 'mkdir -p' command.
 *
 *  All directories created will have access mode set by @a mode, for this
 *  reason the mode should be at least 'rwx------'.
 *
 *  @param[in]  path            The path to the directory to create.
 *  @param[in]  mode            The file access mode to give to all
 *                              directories created.
 *
 *  @return true on success, false on failure.
 */
bool DobbyRdkPluginUtils::mkdirRecursive(const std::string& path, mode_t mode)
{
    AI_LOG_FN_ENTRY();

    if (path.empty())
    {
        AI_LOG_ERROR_EXIT("empty path supplied");
        return false;
    }

    std::istringstream stream(path);
    std::string token;

    std::string partial((path.front() == '/') ? "/" : "");
    while (std::getline(stream, token, '/'))
    {
        if (token.empty())
            continue;

        partial += token;
        partial += '/';

        if (mkdir(partial.c_str(), mode) != 0)
        {
            if (errno == EEXIST)
                continue;

            AI_LOG_SYS_ERROR_EXIT(errno, "failed to create dir '%s'",
                                  partial.c_str());
            return false;
        }

        if (chmod(partial.c_str(), mode) != 0)
        {
            AI_LOG_SYS_ERROR_EXIT(errno, "failed to set dir '%s' perms to 0%03o",
                                  partial.c_str(), mode);
            return false;
        }
    }

    AI_LOG_FN_EXIT();
    return true;
}

// -----------------------------------------------------------------------------
/**
 *  @brief Public api to allow for adding additional environment variables
 *
 *  This can only obviously be called before the config file is persisted to
 *  disk.
 *
 *  @param[in]  cfg         Pointer to OCI config struct
 *  @param[in]  envVar      The environment variable to set
 *
 *  @return true if the env var was added, otherwise false.
 */
bool DobbyRdkPluginUtils::addEnvironmentVar(const std::string& envVar) const
{
    AI_LOG_FN_ENTRY();

    std::lock_guard<std::mutex> locker(mLock);

    // check if env var already exists in config
    for (int i = 0; i < mConf->process->env_len; ++i)
    {
        if (0 == strcmp(mConf->process->env[i], envVar.c_str()))
        {
            return true;
        }
    }

    // Increase the number of environmental variables
    mConf->process->env_len += 1;

    // Update env var in OCI bundle config
    mConf->process->env = (char**)realloc(mConf->process->env, sizeof(char*) * mConf->process->env_len);
    mConf->process->env[mConf->process->env_len-1] = strdup(envVar.c_str());

    AI_LOG_FN_EXIT();
    return true;
}