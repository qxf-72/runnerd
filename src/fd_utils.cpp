#include "runnerd/fd_utils.h"

#include <fcntl.h>

#include <cerrno>
#include <system_error>

namespace runnerd {

void setNonBlocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);

  if (flags == -1) {
    const int error_number = errno;
    throw std::system_error(error_number, std::generic_category(), "fcntl(F_GETFL)");
  }

  if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
    const int error_number = errno;
    throw std::system_error(error_number, std::generic_category(), "fcntl(F_SETFL)");
  }
}

}  // namespace runnerd
