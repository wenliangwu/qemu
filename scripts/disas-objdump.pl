#!/usr/bin/env perl

# USAGE
#
# This code will insert dissassembled assembly and C code against qemu trace
# output. Very good for debugging alongside the fuzzer.
#
# 1) First run qemu with your SOF kernel (use -r or -b options if needed for
#    ROM or bootloader symbols.
#
#  ./xtensa-host.sh bdw -k ../sof/build_bdw_gcc/sof-bdw.ri  -t > trace-file 2>&1
#
# 2) Run the fuzzer with tests and optionally attach GDB to qemu.
#
# 3) Now take the trace file and add the source
#   ./scripts/disas-objdump.pl -t xtensa -p xtensa-hsw-elf -k ../sof/build_bdw_gcc/sof trace-file

use warnings;

use File::Temp qw/ tempfile /;
use Getopt::Long;

# Default to the system objdump if a cross-compiler edition not given.
my $aobjdump = "";
my $hobjdump = "";
my $tobjdump = "";
my $hmachine = "";
my $tmachine = "";
my $ksymbols = "";
my $bsymbols = "";
my $rsymbols = "";
my $prefix = "";

GetOptions ('O|objdump=s' => \$aobjdump,
            'host-objdump=s' => \$hobjdump,
            'target-objdump=s' => \$tobjdump,
            'h|host-machine=s' => \$hmachine,
            't|target-machine=s' => \$tmachine,
	    'k|ksymbols=s' => \$ksymbols,
	    'r|rsymbols=s' => \$rsymbols,
	    'b|bsymbols=s' => \$bsymbols,
	    'p|prefix=s' => \$prefix);

# But we can't default the machines.  Sanity check that we've at least one.
die "No host or target machine type" if !$hmachine && !$tmachine;

$aobjdump = $prefix . "-objdump";

# Reuse one temp file for all of the hunks.
my ($outh, $outname) = tempfile();
binmode($outh);
END { unlink $outname; }

# Pre-construct the command-lines for executing the dump.
sub mkobjcommand ($$) {
    my ($cmd, $mach) = @_;
    return 0 if !$mach;
    $cmd = $aobjdump if !$cmd;
    return "$cmd -m $mach --disassemble-all -b binary";
}

# Pre-construct the command-lines for executing the dump.
sub mksrccommand ($$) {
    my ($infile, $vma) = @_;
    my $cmd = $aobjdump;
    return "$cmd --start-address $vma -S $infile";
}

$objdump[1] = mkobjcommand($hobjdump, $hmachine);
$objdump[2] = mkobjcommand($tobjdump, $tmachine);

# Zero-initialize current dumping state.
my $mem = "";
my $inobjd = 0;
my $vma = 0;

sub objcommand {
    my $ret = $objdump[$inobjd];
    if (!$ret) {
        die "Host machine type not specified" if $inobjd == 1;
        die "Target machine type not specified" if $inobjd == 2;
        die "Internal error";
    }
    return $ret;
}

while (<>) {
    # Collect the data from the relevant OBJD-* lines ...
    if (/^OBJD-H: /) {
        die "Internal error" if $inobjd == 2;
        $mem = $mem . pack("H*", substr($_, 8, -1));
        $inobjd = 1;
    } elsif (/^OBJD-T: /) {
        die "Internal error" if $inobjd == 1;
        $mem = $mem . pack("H*", substr($_, 8, -1));
        $inobjd = 2;
    }
    # ... which will always be followed by a blank line,
    # at which point we should produce our dump.
    elsif ($inobjd) {
        # Rewrite the temp file in one go; it will usually be small.
        sysseek $outh, 0, 0;
        truncate $outh, 0;
        syswrite $outh, $mem;

        my $cmd = objcommand();
        $cmd = $cmd . " --adjust-vma=" . $vma if $vma;
        $cmd = $cmd . " " . $outname;

	my $addr = substr($vma, 2, 6);
	my $kcmd = mksrccommand($ksymbols, $vma);
	my $bcmd = mksrccommand($bsymbols, $vma);
	my $rcmd = mksrccommand($rsymbols, $vma);

        # Pipe from objdump...
        open IN, "-|", $cmd;

        # ... copying all but the first 7 lines of boilerplate to our stdout.
	my $i = 0;
	while (<IN>) {
	    print if (++$i > 7);
        }
        close IN;
        print "\n";

	# get kernel source
	if ($ksymbols) {
		my $enable = 0;

		# Pipe from objdump...
		open IN, "-|", $kcmd;

		# ... copying 10, but the first 7 lines of boilerplate to our stdout.
		my $i = 0;
		while (<IN>) {

			if (index($_, $addr) != -1) {
				$enable++;
			}

			print if (++$i > 7 && $i < 17 && $enable > 0);

		}

		close IN;
	}

	# get bootloader source
	if ($bsymbols) {
		my $enable = 0;

		# Pipe from objdump...
		open IN, "-|", $bcmd;

		# ... copying 10, but the first 7 lines of boilerplate to our stdout.
		my $i = 0;
		while (<IN>) {

			if (index($_, $addr) != -1) {
				$enable++;
			}

			print if (++$i > 7 && $i < 17 && $enable > 0);
			}
		close IN;
	}

	# get ROM source
	if ($rsymbols) {
		my $enable = 0;

		# Pipe from objdump...
		open IN, "-|", $rcmd;

		# ... copying 10, but the first 7 lines of boilerplate to our stdout.
		my $i = 0;
		while (<IN>) {

			if (index($_, $addr) != -1) {
				$enable++;
			}

			print if (++$i > 7 && $i < 17 && $enable > 0);
			}
		close IN;
	}

        $mem = "";
        $inobjd = 0;
        $vma = 0;
	 print "\n";
    }
    # The line before "OBJD-*" will be of the form "0x<hex>+: +\n".
    # Extract the value for passing to --adjust-vma.
    elsif (/^(0x[0-9a-fA-F]+):\s*$/) {
        $vma = $1;
        print;
    } else {
        print;
    }
}
