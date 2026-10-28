#pragma once

#include <string_view>

namespace ducat {

// Whether a file name looks like an archive that can be listed: zip, 7z, rar,
// tar (also compressed: .tar.gz, .tgz, .tar.xz…), cpio, iso, cab, deb and
// rpm, in any case. Files merely compressed (.gz, .xz) and formats built on
// zip for other purposes (.jar, .odt, .docx) do not count. Only the name is
// looked at, as for a stored entry whose drive may be gone.
bool is_archive_name(std::string_view name);

} // namespace ducat
