dnl
dnl This is a cloning of _AM_PROG_TAR in aclocal/tar.m4,
dnl although its namespace is FT_XXX.
dnl
dnl
dnl FT_PROG_TAR()
dnl  $1 = space separated commands to be tried
dnl       (default: gnutar gtar bsdtar tar pax cpio)
dnl
dnl  The tested results are recorded in:
dnl    ft_tar_cmd:   the command to emit POSIX ustar
dnl                  it can be the commands whose syntax
dnl                  is incompatible with tar - like,
dnl                  pax or cpio.
dnl    ft_tar_flags: the options to emit POSIX ustar
dnl    ft_tar_chf_:  the command with flags to emit
dnl                  ustar bitstream from $(tardir)
dnl                  to STDOUT.
dnl    ft_tar_xf_:   the command with flags to extract
dnl                  ustar bitstream from STDIN.
dnl
dnl Example of usage in configure.ac
dnl
dnl   FT_PROG_TAR([gnutar gtar bsdtar tar pax cpio])
dnl   AC_SUBST([TAR_CHF_],[${ft_tar_chf_}])
dnl   AC_SUBST([UNTAR_XF_],[${ft_tar_xf_}])
dnl
dnl Example of usage in Makefile.am
dnl
dnl   freetype-$(version).tar:
dnl       ...
dnl       tardir=freetype-$(version) && $(TAR_CHF_) > $@
dnl
dnl
AC_DEFUN([FT_PROG_TAR],[
  tar_candidates="$1"
  if test -z "${tar_candidates}"
  then
    tar_candidates="gnutar gtar bsdtar tar pax cpio"
  fi

  AC_MSG_CHECKING([tar supporting ustar and following symlink])

  unset ft_tarflags
  for ft_tar_cmd in ${tar_candidates}
  do
    case ${ft_tar_cmd} in
      *tar)
        ft_tar_cflags="--format=ustar -chf -"
        ft_tar_xflags="--format=ustar -xf -"
        ;;
      *pax)
        ft_tar_cflags="-w -x ustar -L"
        ft_tar_xflags="-r"
        ;;
      *cpio)
        ft_tar_cflags="-o -H ustar -L"
        ft_tar_xflags="-i -H ustar"
        ;;
      *)
        AC_MSG_WARN([cannot test ${ft_tar_cmd}])
        continue
        ;;
    esac

    if expr "${ft_tar_cmd}" : ".*cpio" > /dev/null
    then
      ft_tar_ok=`(echo . | ${ft_tar_cmd} ${ft_tar_cflags} > /dev/null 2>/dev/null && echo yes) || echo no`
      ft_tar_chf_='find $${tardir} -print | '"${ft_tar_cmd} ${ft_tar_cflags}"
    else
      ft_tar_ok=`(${ft_tar_cmd} ${ft_tar_cflags} . > /dev/null 2>/dev/null && echo yes) || echo no`
      ft_tar_chf_="${ft_tar_cmd} ${ft_tar_cflags} "'$${tardir}'
    fi

    if test "x${ft_tar_ok}" = xyes
    then
      AC_MSG_RESULT([found, "${ft_tar_cmd} ${ft_tar_cflags}"])
      break
    fi
  done

  if test "x${ft_tar_ok}" != xyes
  then
    AC_MSG_RESULT([not found, fallback plain tar])
    unset ft_tar_cmd
    AC_CHECK_PROG([ft_tar_cmd],[tar],[tar],[false])
    if test "x${ft_tar_cmd}" = xtar
    then
      ft_tar_cflags="chf -"
      ft_tar_xflags="xf -"
      ft_tar_chf_='tar chf - $${tardir}'
      ft_tar_xf_='tar xf -'
    else
      AC_MSG_WARN(["tar" command is missing, "make dist" will fail])
      ft_tar_cflags=""
      ft_tar_xflags=""
      ft_tar_chf_=false
      ft_tar_xf_=false
    fi
  fi
])
