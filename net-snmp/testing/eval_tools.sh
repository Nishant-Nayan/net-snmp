#
# eval_tools.sh
#
# Output functions for script tests.  Source this from other test scripts
# to establish a standardized repertory of test functions.
#
#
# Except where noted, all functions return:
#	0	On success,	(Bourne Shell's ``true'')
#	non-0	Otherwise.
#
# Input arguments to each function are documented with each function.
#
#
# XXX  Suggestions:
#	DEBUG ON|OFF
#	dump CAPTURE output to stdout as well as to junkoutputfile.
#

#
# Only allow ourselves to be eval'ed once
#
if [ "x$EVAL_TOOLS_SH_EVALED" != "xyes" ]; then
    EVAL_TOOLS_SH_EVALED=yes
    . ../TESTCONF.sh

#
# Variables used in global environment of calling script.
#
failcount=0
junkoutputfile="$SNMP_TMPDIR/output-`basename $0`$$"

#
# HEADER: returns a single line when SNMP_HEADERONLY mode and exits.
#
HEADER() {
    if [ "x$SNMP_HEADERONLY" != "x" ]; then
        echo test $*
	exit 0;
    else
	echo -n "testing $*...  "
    fi
}


#------------------------------------ -o- 
#
OUTPUT() {	# <any_arguments>
	cat <<GRONK


$*


GRONK
}


#------------------------------------ -o- 
#
SUCCESS() {	# <any_arguments>
	[ "$failcount" -ne 0 ] && return
	cat <<GROINK

SUCCESS: $*

GROINK
}



#------------------------------------ -o- 
#
FAILED() {	# <return_value>, <any_arguments>
	[ "$1" -eq 0 ] && return
	shift

	failcount=`expr $failcount + 1`
	cat <<GRONIK

FAILED: $*

GRONIK
}


#------------------------------------ -o- 
#
VERIFY() {	# <path_to_file(s)>
	local	missingfiles=

	for f in $*; do
		[ -e "$f" ] && continue
		echo "FAILED: Cannot find file \"$f\"."
		missingfiles=true
	done

	[ "$missingfiles" = true ] && exit 1000
}


#------------------------------------ -o- 
#
STARTTEST() {	
	[ ! -e "$junkoutputfile" ] && {
		touch $junkoutputfile
		return
	}
	echo "FAILED: Output file already exists: \"$junkoutputfile\"."
	exit 1000
}


#------------------------------------ -o- 
#
STOPTEST() {
	rm -rf "$junkoutputfile"
}


#------------------------------------ -o- 
#
CAPTURE() {	# <command_with_arguments_to_execute>
	if [ $SNMP_VERBOSE -gt 0 ]; then
		cat <<KNORG

EXECUTING: $*

KNORG

	fi
	( $* 2>&1 ) > $junkoutputfile

	if [ $SNMP_VERBOSE -gt 1 ]; then
		echo "Output: "
		cat $junkoutputfile | sed 's/^/  /'
	fi
}


#
# Checks the output result against what we expect.
#   Sets return_value to 0 or 1.
#
EXPECTRESULT() {
    if [ $snmp_last_test_result = $1 ]; then
	return_value=0
    else
	return_value=1
    fi
}

#------------------------------------ -o- 
# Returns: Count of matched lines.
#
CHECK() {	# <pattern_to_match>
	if [ $SNMP_VERBOSE -gt 0 ]; then
	    echo -n "checking output for \"$*\"..."
	fi

	rval=`grep -c "$*" "$junkoutputfile" 2>/dev/null`

	if [ $SNMP_VERBOSE -gt 0 ]; then
	    echo "$rval matches found"
	fi

	snmp_last_test_result=$rval
	EXPECTRESULT 1  # default
	return $rval
}


#------------------------------------ -o- 
# Returns: Count of matched lines.
#
CHECKEXACT() {	# <pattern_to_match_exactly>
	rval=`grep -wc "$*" "$junkoutputfile" 2>/dev/null`
	snmp_last_test_result=$rval
	EXPECTRESULT 1  # default
	return $rval
}

CONFIGAGENT() {
    if [ "x$SNMP_CONFIG_FILE" = "x" ]; then
	echo "$0: failed because var: SNMP_CONFIG_FILE wasn't set"
	exit 1;
    fi
    echo $* >> $SNMP_CONFIG_FILE
}

STARTAGENT() {
    snmpd $SNMP_SNMPD_PORT -P $SNMP_SNMPD_PID_FILE -l $SNMP_SNMPD_LOG_FILE -C -c $SNMP_CONFIG_FILE
}

STOPAGENT() {
    if [ -f $SNMP_SNMPD_PID_FILE ]; then
	kill `cat $SNMP_SNMPD_PID_FILE`
	# XXX: kill -9 later (after sleep and ps grok?)?
    fi
    rm $SNMP_SNMPD_PID_FILE
}

FINISHED() {
#    sleep 5
    if [ $SNMP_SAVE_TMPDIR != "xyes" ]; then
	rm -rf $SNMP_TMPDIR
    fi
    if [ "x$return_value" = "x0" ]; then
	echo "ok"
    else
	echo "FAIL"
    fi
    exit $return_value
	
}

fi # Only allow ourselves to be eval'ed once
