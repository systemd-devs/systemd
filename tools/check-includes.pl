# SPDX-License-Identifier: CC0-1.0
#!/usr/bin/env perl
#
# checkincludes: Find files included more than once in (other) files.

foreach $file (@ARGV) {
	open(FILE, $file) or die "Cannot open $file: $!.\n";

	my %includedfiles = ();

	while (<FILE>) {
		if (m/^\s*#\s*include\s*[<"](\S*)[>"]/o) {
			++$includedfiles{$1};
		}
	}
	foreach $filename (keys %includedfiles) {
		if ($includedfiles{$filename} > 1) {
			print "$file: $filename is included more than once.\n";
		}
	}

	close(FILE);
}
