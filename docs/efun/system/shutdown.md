---
layout: doc
title: system / shutdown
---
# shutdown

### NAME

    shutdown() - shutdown the driver

### SYNOPSIS

    void shutdown( int how: 0 );

### DESCRIPTION

    This function shuts down the driver in a controlled fashion (as opposed
    to how a crash would shut it down).  The 'how' argument  specifes  what
    integer  value that driver should pass to exit().  The convention is to
    pass 'how' as -1 when the script that restarts the driver should die as
    well.   Thus a reboot command would use shutdown() while a halt command
    would use shutdown(-1).  The script must explicitly  check  the  return
    value  to  see  if  it is -1 if you wish to use this convention.  Obvi‐
    ously, shutdown() is a sensitive function and should  be  secured.   As
    with  exec(),  the  way to make it secure is to add a simul_efun of the
    same name which does the appropriate security checks.  Be sure  to  set
    valid_override(4) up (in master.c) to protect against efun::shutdown().

### SEE ALSO

    crash(4)

