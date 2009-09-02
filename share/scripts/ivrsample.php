#!/usr/bin/php -q
<?
/* Simple example for the object oriented IVR interface

   To use it you must route a number to:
   external/nodata/ivrsample.php
*/
require_once("libyateivr.php");

class IVR1 extends IVR
{

    // do initialization when entering this IVR
    function OnEnter($state)
    {
	// initialize the operation table on entering the IVR
	$this->optable = array(
	    // key 0 - play back some DTMF keys inband
	    ":0" => "dtmf:123:inband",
	    // key 5 - call the 2nd IVR state 'b', allow it to return
	    ":5" => "call:ivr2:b",
	    // key 6 - jump to 2nd IVR state 'b', will not return to us
	    ":6" => "jump:ivr2:b",
	    // key 8 - leave this IVR, return to parent or exit IVR::Run
	    ":8" => "leave",
	    // key 9 - clear play queue, add 2 files and start playing
	    ":9" => "play:file1.au:file2.slin:clear",
	    // on call.execute immediately answer the call
	    ":execute" => "answered",
	    // when entering this IVR output a message to logs
	    ":enter" => "output:Entered 1st IVR"
	);
	parent::OnEnter($state);
	// call the 2nd IVR
	IVR::Call("ivr2");
    }

    // handle keypad tones while in this IVR
    function OnDTMF($tone)
    {
	$this->Output("Got $tone");
	if (parent::OnDTMF($tone))
	    return true;
	if ($tone == "#")
	    IVR::Call("ivr2");
	return true;
    }
}

class TheIVR_2 extends IVR
{
    // do initialization when entering this IVR
    function OnEnter($state)
    {
	parent::OnEnter($state);
	// this will fail so we remain the current IVR
	IVR::Call("nosuch");
    }

    // handle keypad tones while in this IVR
    function OnDTMF($tone)
    {
	switch ($tone) {
	    case "*":
		// hang up the IVR channel and possibly the incoming call
		IVR::Hangup();
		break;
	    case "#":
		// return to calling IVR with value 'Got #'
		IVR::Leave("Got $tone");
		break;
	    case "0":
	    case "1":
	    case "2":
		$this->Output("Got $tone");
		break;
	    default:
		return false;
	}
	return true;
    }
}

// initialize the Yate PHP library with default values, enable debugging
Yate::Init();
Yate::Debug(true);

// register the two IVR classes with some names
IVR::Register("ivr1","IVR1");
IVR::Register("ivr2","TheIVR_2");

// and start running by entering the 1st IVR
IVR::Run("ivr1");

// if we reach here the IVR is terminated
Yate::Output("PHP: bye!");

/* vi: set ts=8 sw=4 sts=4 noet: */
?>
