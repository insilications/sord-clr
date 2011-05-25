#!/usr/bin/env python
import glob
import os

from waflib.extras import autowaf as autowaf
import waflib.Logs as Logs, waflib.Options as Options

# Version of this package (even if built as a child)
SORD_VERSION       = '0.4.1'
SORD_MAJOR_VERSION = '0'

# Library version (UNIX style major, minor, micro)
# major increment <=> incompatible changes
# minor increment <=> compatible changes (additions)
# micro increment <=> no interface changes
# Sord uses the same version number for both library and package
SORD_LIB_VERSION = SORD_VERSION

# Variables for 'waf dist'
APPNAME = 'sord'
VERSION = SORD_VERSION

# Mandatory variables
top = '.'
out = 'build'

def options(opt):
    autowaf.set_options(opt)
    opt.add_option('--test', action='store_true', default=False, dest='build_tests',
                   help="Build unit tests")
    opt.add_option('--dump', type='string', default='', dest='dump',
                   help="Dump debugging output (iter, search, write, all)")

def configure(conf):
    autowaf.configure(conf)
    conf.line_just = 32
    autowaf.display_header('Sord configuration')

    conf.load('compiler_cc')
    conf.env.append_value('CFLAGS', '-std=c99')

    autowaf.check_pkg(conf, 'glib-2.0', uselib_store='GLIB',
                      atleast_version='2.0.0', mandatory=True)

    autowaf.check_pkg(conf, 'serd-0', uselib_store='SERD',
                      atleast_version='0.4.0', mandatory=True)

    conf.env['BUILD_TESTS'] = Options.options.build_tests
    conf.env['BUILD_UTILS'] = True

    dump = Options.options.dump.split(',')
    all = 'all' in dump
    if all or 'iter' in dump:
        autowaf.define(conf, 'SORD_DEBUG_ITER', 1)
    if all or 'search' in dump:
        autowaf.define(conf, 'SORD_DEBUG_SEARCH', 1)
    if all or 'write' in dump:
        autowaf.define(conf, 'SORD_DEBUG_WRITE', 1)

    autowaf.define(conf, 'SORD_VERSION', SORD_VERSION)
    conf.write_config_header('sord-config.h', remove=False)

    conf.env['INCLUDES_SORD'] = ['%s/sord-%s' % (
        conf.env['INCLUDEDIR'], SORD_MAJOR_VERSION)]

    if not conf.env['INCLUDES_SERD']:
        # Not sure why/how this happens, but assume serd is in the same prefix
        Logs.warn("Warning: INCLUDES_SERD not found in environment, odd...\n")
        conf.env['INCLUDES_SORD'] = ['%s/serd-0' % conf.env['INCLUDEDIR']]
    conf.env['LIBPATH_SORD'] = [conf.env['LIBDIR']]
    conf.env['LIB_SORD'] = ['sord-%s' % SORD_MAJOR_VERSION];

    autowaf.display_msg(conf, "Utilities", bool(conf.env['BUILD_UTILS']))
    autowaf.display_msg(conf, "Unit tests", bool(conf.env['BUILD_TESTS']))
    autowaf.display_msg(conf, "Debug dumping", dump)
    print('')

def build(bld):
    # C/C++ Headers
    includedir = '${INCLUDEDIR}/sord-%s/sord' % SORD_MAJOR_VERSION
    bld.install_files(includedir, bld.path.ant_glob('sord/*.h'))
    bld.install_files(includedir, bld.path.ant_glob('sord/*.hpp'))

    # Pkgconfig file
    autowaf.build_pc(bld, 'SORD', SORD_VERSION, SORD_MAJOR_VERSION, 'SERD',
                     {'SORD_MAJOR_VERSION' : SORD_MAJOR_VERSION})

    # Library
    obj = bld(features        = 'c cshlib',
              source          = 'src/sord.c src/syntax.c',
              includes        = ['.', './src'],
              export_includes = ['.'],
              name            = 'libsord',
              target          = 'sord-%s' % SORD_MAJOR_VERSION,
              vnum            = SORD_LIB_VERSION,
              install_path    = '${LIBDIR}',
              libs            = [ 'm' ],
              cflags          = [ '-fvisibility=hidden',
                                  '-DSORD_SHARED',
                                  '-DSORD_INTERNAL' ])
    autowaf.use_lib(bld, obj, 'GLIB SERD')

    if bld.env['BUILD_TESTS']:
        test_cflags = [ '-fprofile-arcs',  '-ftest-coverage' ]

        # Static library (for unit test code coverage)
        obj = bld(features     = 'c cstlib',
                  source       = 'src/sord.c src/syntax.c',
                  includes     = ['.', './src'],
                  name         = 'libsord_static',
                  target       = 'sord_static',
                  install_path = '',
                  cflags       = test_cflags,
                  libs         = [ 'm' ])
        autowaf.use_lib(bld, obj, 'GLIB SERD')

        # Unit test program
        obj = bld(features     = 'c cprogram',
                  source       = 'src/sord_test.c',
                  includes     = ['.', './src'],
                  use          = 'libsord_static',
                  linkflags    = '-lgcov',
                  target       = 'sord_test',
                  install_path = '',
                  cflags       = test_cflags)
        autowaf.use_lib(bld, obj, 'GLIB SERD')

        # Static sordi build
        obj = bld(features = 'c cprogram')
        obj.source       = 'src/sordi.c'
        obj.includes     = ['.', './src']
        obj.use          = 'libsord_static'
        obj.linkflags    = '-lgcov'
        obj.target       = 'sordi_static'
        obj.install_path = ''
        obj.cflags       = [ '-fprofile-arcs',  '-ftest-coverage' ]

        # C++ build test
        obj = bld(features     = 'cxx cxxprogram',
                  source       = 'src/sordmm_test.cpp',
                  includes     = ['.', './src'],
                  use          = 'libsord_static',
                  linkflags    = '-lgcov',
                  target       = 'sordmm_test',
                  install_path = '',
                  cflags       = test_cflags)
        autowaf.use_lib(bld, obj, 'GLIB SERD')

    # Static command line utility (for testing)
        if bld.env['BUILD_UTILS']:
            obj = bld(features     = 'c cprogram',
                      source       = 'src/sordi.c',
                      includes     = ['.', './src'],
                      use          = 'libsord_static',
                      linkflags    = '-lgcov',
                      target       = 'sordi_static',
                      install_path = '',
                      cflags       = test_cflags)

    # Command line utility
    if bld.env['BUILD_UTILS']:
        obj = bld(features     = 'c cprogram',
                  source       = 'src/sordi.c',
                  includes     = ['.', './src'],
                  use          = 'libsord',
                  linkflags    = '-lgcov',
                  target       = 'sordi',
                  install_path = '${BINDIR}')

    # Documentation
    autowaf.build_dox(bld, 'SORD', SORD_VERSION, top, out)

    # Man page
    bld.install_files('${MANDIR}/man1', 'doc/sordi.1')

    bld.add_post_fun(autowaf.run_ldconfig)
    if bld.env['DOCS']:
        bld.add_post_fun(fix_docs)

def build_dir(ctx, subdir):
    if autowaf.is_child():
        return os.path.join('build', APPNAME, subdir)
    else:
        return os.path.join('build', subdir)
    
def fix_docs(ctx):
    try:
        top = os.getcwd()
        os.chdir(build_dir(ctx, 'doc/html'))
        os.system("sed -i 's/SORD_API //' group__sord.html")
        os.system("sed -i 's/SORD_DEPRECATED //' group__sord.html")
        os.remove('index.html')
        os.symlink('group__sord.html',
                   'index.html')
        os.chdir(top)
        os.chdir(build_dir(ctx, 'doc/man/man3'))
        os.system("sed -i 's/SORD_API //' sord.3")
        os.chdir(top)
    except:
        Logs.error("Failed to fix up %s documentation" % APPNAME)

def upload_docs(ctx):
    os.system("rsync -ravz --delete -e ssh build/doc/html/ drobilla@drobilla.net:~/drobilla.net/docs/sord/")

def test(ctx):
    blddir = build_dir(ctx, 'tests')
    try:
        os.makedirs(blddir)
    except:
        pass

    for i in glob.glob(blddir + '/*.*'):
        os.remove(i)

    srcdir   = ctx.path.abspath()
    orig_dir = os.path.abspath(os.curdir)

    os.chdir(srcdir)

    good_tests = glob.glob('tests/test-*.ttl')
    good_tests.sort()

    os.chdir(orig_dir)

    autowaf.pre_test(ctx, APPNAME)

    autowaf.run_tests(ctx, APPNAME, [
            './sordi_static file:%s/tests/manifest.ttl > /dev/null' % srcdir,
            './sordi_static file://%s/tests/manifest.ttl > /dev/null' % srcdir,
            './sordi_static %s/tests/UTF-8.ttl > /dev/null' % srcdir,
            './sordi_static -v > /dev/null',
            './sordi_static -h > /dev/null',
            './sordi_static -s "<foo> a <#Thingie> ." > /dev/null',
            './sordi_static /dev/null > /dev/null'],
                      0, name='sordi-cmd-good')

    autowaf.run_tests(ctx, APPNAME, [
            './sordi_static > /dev/null',
            './sordi_static ftp://example.org/unsupported.ttl > /dev/null',
            './sordi_static -i > /dev/null',
            './sordi_static -o > /dev/null',
            './sordi_static -z > /dev/null',
            './sordi_static -p > /dev/null',
            './sordi_static -c > /dev/null',
            './sordi_static -i illegal > /dev/null',
            './sordi_static -o illegal > /dev/null',
            './sordi_static -i turtle > /dev/null',
            './sordi_static /no/such/file > /dev/null'],
                      1, name='sordi-cmd-bad')

    autowaf.run_tests(ctx, APPNAME, ['./sord_test'])

    commands = []
    for test in good_tests:
        base_uri = 'http://www.w3.org/2001/sw/DataAccess/df1/' + test
        commands += [ './sordi_static %s/%s \'%s\' > %s.out' % (srcdir, test, base_uri, test) ]

    autowaf.run_tests(ctx, APPNAME, commands, 0, name='good')

    Logs.pprint('BOLD', '\nVerifying turtle => ntriples')
    for test in good_tests:
        out_filename = test + '.out'
        cmp_filename = srcdir + '/' + test.replace('.ttl', '.out')
        if not os.access(out_filename, os.F_OK):
            Logs.pprint('RED', 'FAIL: %s output is missing' % test)
        else:
            out_lines = sorted(open(out_filename).readlines())
            cmp_lines = sorted(open(cmp_filename).readlines())
            if out_lines != cmp_lines:
                Logs.pprint('RED', 'FAIL: %s is incorrect' % out_filename)
            else:
                Logs.pprint('GREEN', 'Pass: %s' % test)

    autowaf.post_test(ctx, APPNAME)
