// Configure-time check: exits with 0 when Tkrzw really removes records.
#include <tkrzw_dbm_std.h>

int main()
{
    tkrzw::StdTreeDBM dbm;
    dbm.Set("key", "value");
    dbm.Remove("key");
    return dbm.CountSimple() == 0 ? 0 : 1;
}
