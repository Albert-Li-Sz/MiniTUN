#include "file_security.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <minitun/common/error.hpp>

namespace minitun::storage::internal {
namespace {

[[nodiscard]] common::Error posix_error(const int error_number, const std::string_view operation) {
    common::ErrorCode code = common::ErrorCode::database_error;
    if (error_number == EACCES || error_number == EPERM || error_number == ELOOP) {
        code = common::ErrorCode::permission_denied;
    } else if (error_number == ENOENT || error_number == ENOTDIR) {
        code = common::ErrorCode::not_found;
    }
    std::string message{operation};
    message.append(" failed: ");
    message.append(std::strerror(error_number));
    return common::Error{code, std::move(message)};
}

[[nodiscard]] std::string parent_path(const std::string_view path) {
    const auto slash = path.rfind('/');
    if (slash == std::string_view::npos) {
        return ".";
    }
    if (slash == 0U) {
        return "/";
    }
    return std::string{path.substr(0U, slash)};
}

[[nodiscard]] std::string operation(const std::string_view description,
                                    const std::string_view suffix) {
    std::string value{description};
    value.push_back(' ');
    value.append(suffix);
    return value;
}

[[nodiscard]] common::Result<void> verify_status(const struct stat& status,
                                                 const std::string_view description) {
    if (!S_ISREG(status.st_mode) || status.st_uid != ::geteuid() || status.st_nlink != 1) {
        return common::Result<void>::failure(
            common::ErrorCode::permission_denied,
            std::string{description} + " must be a daemon-owned regular file with one link");
    }
    if ((status.st_mode & 0777) != (S_IRUSR | S_IWUSR)) {
        return common::Result<void>::failure(common::ErrorCode::permission_denied,
                                             std::string{description} + " must have mode 0600");
    }
    return common::Result<void>::success();
}

} // namespace

PreparedDatabaseFile::PreparedDatabaseFile(int descriptor, std::string path,
                                           std::string description, const std::uint64_t device,
                                           const std::uint64_t inode) noexcept
    : descriptor_(descriptor), path_(std::move(path)), description_(std::move(description)),
      device_(device), inode_(inode) {}

PreparedDatabaseFile::~PreparedDatabaseFile() noexcept {
    if (descriptor_ >= 0) {
        static_cast<void>(::close(descriptor_));
    }
}

PreparedDatabaseFile::PreparedDatabaseFile(PreparedDatabaseFile&& other) noexcept
    : descriptor_(std::exchange(other.descriptor_, -1)), path_(std::move(other.path_)),
      description_(std::move(other.description_)), device_(other.device_), inode_(other.inode_) {}

common::Result<void> PreparedDatabaseFile::verify_path_identity() const {
    struct stat status{};
    if (::lstat(path_.c_str(), &status) != 0) {
        return posix_error(errno, operation(description_, "path verification"));
    }
    auto valid = verify_status(status, description_);
    if (!valid) {
        return valid;
    }
    if (static_cast<std::uint64_t>(status.st_dev) != device_ ||
        static_cast<std::uint64_t>(status.st_ino) != inode_) {
        return common::Result<void>::failure(common::ErrorCode::permission_denied,
                                             description_ + " changed while it was being opened");
    }
    return common::Result<void>::success();
}

common::Result<PreparedDatabaseFile>
prepare_private_database_file(const std::string_view path, const std::string_view description) {
    const std::string parent = parent_path(path);
    struct stat parent_status{};
    if (::lstat(parent.c_str(), &parent_status) != 0) {
        return common::Result<PreparedDatabaseFile>::failure(
            posix_error(errno, operation(description, "directory inspection")));
    }
    if (!S_ISDIR(parent_status.st_mode) || parent_status.st_uid != ::geteuid() ||
        (parent_status.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        return common::Result<PreparedDatabaseFile>::failure(
            common::ErrorCode::permission_denied,
            std::string{description} +
                " directory must be non-writable by other users and owned by the daemon user");
    }

    int flags = O_RDWR | O_CREAT | O_CLOEXEC;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const std::string owned_path{path};
    const int descriptor =
        ::open(owned_path.c_str(), flags, static_cast<mode_t>(S_IRUSR | S_IWUSR));
    if (descriptor < 0) {
        return common::Result<PreparedDatabaseFile>::failure(
            posix_error(errno, operation(description, "file open")));
    }

    struct stat status{};
    common::Result<void> valid = common::Result<void>::success();
    if (::fstat(descriptor, &status) != 0) {
        valid = posix_error(errno, operation(description, "file inspection"));
    } else if (!S_ISREG(status.st_mode) || status.st_uid != ::geteuid() || status.st_nlink != 1) {
        valid = common::Result<void>::failure(
            common::ErrorCode::permission_denied,
            std::string{description} + " must be a daemon-owned regular file with one link");
    } else if (::fchmod(descriptor, S_IRUSR | S_IWUSR) != 0) {
        valid = posix_error(errno, operation(description, "permission update"));
    } else if (::fstat(descriptor, &status) != 0) {
        valid = posix_error(errno, operation(description, "permission verification"));
    } else {
        valid = verify_status(status, description);
    }
    if (!valid) {
        static_cast<void>(::close(descriptor));
        return common::Result<PreparedDatabaseFile>::failure(valid.error());
    }

    PreparedDatabaseFile prepared{descriptor, owned_path, std::string{description},
                                  static_cast<std::uint64_t>(status.st_dev),
                                  static_cast<std::uint64_t>(status.st_ino)};
    auto identity = prepared.verify_path_identity();
    if (!identity) {
        return common::Result<PreparedDatabaseFile>::failure(identity.error());
    }
    return prepared;
}

} // namespace minitun::storage::internal
