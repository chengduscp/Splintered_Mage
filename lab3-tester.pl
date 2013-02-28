#! /usr/bin/perl -w

open(FOO, "ospfsmod.c") || die "Did you delete ospfsmod.c?";
$lines = 0;
$lines++ while defined($_ = <FOO>);
close FOO;

@tests = (
    # test reading
    # 1
    [ 'diff base/hello.txt test/hello.txt >/dev/null 2>&1 && echo $?',
      "0"
    ],
    
    # 2
    [ 'cmp base/pokercats.gif test/pokercats.gif >/dev/null 2>&1 && echo $?',
      "0"
    ],
    
    # 3 
    [ 'ls -l test/pokercats.gif | awk "{ print \$5 }"',
      "91308"
    ],

    # 4
    # read first byte of a file
    [ 'dd bs=1 if=test/pokercats.gif > /dev/null 2>&1 && echo $?',
      "0"
    ],

    # 5
    # read first block of a file
    [ 'dd bs=1024 if=test/pokercats.gif > /dev/null 2>&1 && echo $?',
      "0"
    ],

    # 6
    # read half of first block of a file
    [ 'dd bs=512 if=test/pokercats.gif > /dev/null 2>&1 && echo $?',
      "0"
    ],

    # 7
    # read partway through first block and part way through second
    [ 'dd bs=1024 seek=624 if=test/pokercats.gif > /dev/null 2>&1 && echo $?',
      "0"
    ],

    # 8
    # try to read past the end of the file
    [ 'dd bs=10 skip=512 if=test/hello.txt > /dev/null 2>&1 && echo $?',
      "0"
    ],

    # test writing
    # We use dd to write because it doesn't initially truncate, and it can
    # be told to seek forward to a particular point in the disk.
    # 9
    [ "echo Bybye | dd bs=1 count=5 of=test/hello.txt conv=notrunc >/dev/null 2>&1 ; cat test/hello.txt",
      "Bybye, world!"
    ],
    
    # 10
    [ "echo Hello | dd bs=1 count=5 of=test/hello.txt conv=notrunc >/dev/null 2>&1 ; cat test/hello.txt",
      "Hello, world!"
    ],
    
    # 11
    [ "echo gi | dd bs=1 count=2 seek=7 of=test/hello.txt conv=notrunc >/dev/null 2>&1 ; cat test/hello.txt",
      "Hello, girld!"
    ],
    
    # 12
    [ "echo worlds galore | dd bs=1 count=13 seek=7 of=test/hello.txt conv=notrunc >/dev/null 2>&1 ; cat test/hello.txt",
      "Hello, worlds galore"
    ],
    
    # 13
    # overwrite the part of the first block and part of the second block of a file
    [ 'cat test/direct.txt | dd bs=653 count=2 seek=1 of=test/indirect.txt conv=notrunc > /dev/null 2>&1 &&' .
     ' diff test/indirect.txt ./indirect.txt',
      ""
    ],

    # 14
    [ "echo 'Hello, world!' > test/hello.txt ; cat test/hello.txt",
      "Hello, world!"
    ],

    # 15
    # overwrite first two blocks of the file
    [ 'cat test/direct.txt | dd bs=1024 count=1 of=test/indirect.txt conv=notrunc > /dev/null 2>&1 && echo $?',
      "0"
    ],

    # 16
    # overwrite the second block of the file, not the first
    [ 'cat test/direct.txt | dd bs=1024 count=1 seek=1 of=test/indirect.txt conv=notrunc > /dev/null 2>&1 && echo $?',
      "0"
    ],

    # 17
    # odd sized writing block
    [ 'cat test/direct.txt | dd bs=1536 count=1 of=test/indirect.txt conv=notrunc > /dev/null 2>&1 && echo $?',
      "0"
    ],

    # 18
    # overwrite the middle of the first block of the file
    [ 'cat test/direct.txt | dd bs=256 count=4 seek=1 of=test/indirect.txt conv=notrunc > /dev/null 2>&1 && echo $?',
      "0"
    ],

    # 19
    # overwrite the second half of the first block of a file
    [ 'cat test/direct.txt | dd bs=512 count=2 seek=1 of=test/indirect.txt conv=notrunc > /dev/null 2>&1 && echo $?',
      "0"
    ],
    

    # 20
    # create a file
    [ 'touch test/file1 && echo $?',
      "0"
    ],

    # 21
    # read directory
    [ 'touch test/dir-contents.txt ; ls test | tee test/dir-contents.txt | grep file1',
      'file1'
    ],

    # 22
    # write files, remove them, then read dir again
    [ 'ls test | dd bs=1 of=test/dir-contents.txt >/dev/null 2>&1; ' .
      ' touch test/foo test/bar test/baz && '.
      ' rm    test/foo test/bar test/baz && '.
      'diff <( ls test ) test/dir-contents.txt',
      ''
    ],

    # 23
    # remove the last file
    [ 'rm -f test/dir-contents.txt && ls test | grep dir-contents.txt',
      ''
    ],

    # 24
    # write to a file
    [ 'echo hello > test/file1 && cat test/file1',
      'hello'
    ],
    
    # 25
    # append to a file
    [ 'echo hello > test/file1 ; echo goodbye >> test/file1 && cat test/file1',
      'hello goodbye'
    ],

    # 26
    # delete a file
    [ 'rm -f test/file1 && ls test | grep file1',
      ''
    ],

    # 27
    # make a larger file for indirect blocks
    [ 'yes | head -n 5632 > test/yes.txt && ls -l test/yes.txt | awk \'{ print $5 }\'',
      '11264'
    ],

    # 28
    # truncate the large file
    [ 'echo truncernated11 > test/yes.txt | ls -l test/yes.txt | awk \'{ print $5 }\' ; rm test/yes.txt',
      '15'
    ],

    # 29
    # hard link a file
    [ 'ln test/overwrite.txt test/hardlink && diff test/overwrite.txt test/hardlink',
      ''
    ],

    # 30
    # delete a hard link from a file
    

    # 31
    # soft link creation
    [ 'ln -s test/overwrite.txt test/softlink && ls -l test/softlink | awk \'{ print $10 }\'',
      'test/overwrite.txt'
    ],
    
    # 32
    # remove a symbolic link
);

my($ntest) = 0;
my(@wanttests);

foreach $i (@ARGV) {
    $wanttests[$i] = 1 if (int($i) == $i && $i > 0 && $i <= @tests);
}

my($sh) = "bash";
my($tempfile) = "lab3test.txt";
my($ntestfailed) = 0;
my($ntestdone) = 0;

foreach $test (@tests) {
    $ntest++;
    next if (@wanttests && !$wanttests[$ntest]);
    $ntestdone++;
    print STDOUT "Running test $ntest\n";
    my($in, $want) = @$test;
    open(F, ">$tempfile") || die;
    print F $in, "\n";
    print STDERR "  ", $in, "\n";
    close(F);
    $result = `$sh < $tempfile 2>&1`;
    $result =~ s|\[\d+\]||g;
    $result =~ s|^\s+||g;
    $result =~ s|\s+| |g;
    $result =~ s|\s+$||;

    next if $result eq $want;
    next if $want eq 'Syntax error [NULL]' && $result eq '[NULL]';
    next if $result eq $want;
    print STDERR "Test $ntest FAILED!\n  input was \"$in\"\n  expected output like \"$want\"\n  got \"$result\"\n";
    $ntestfailed += 1;
}

unlink($tempfile);
my($ntestpassed) = $ntestdone - $ntestfailed;
print "$ntestpassed of $ntestdone tests passed\n";
exit(0);
