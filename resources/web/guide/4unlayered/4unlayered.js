// Unlayered 2026-10-09: one page for stealth mode AND the Bambu Lab network plug-in (see index.html).
// Both answers are reported on load (so the defaults count even if nothing is clicked), on every
// click, and once more just before Finish.

function OnInit()
{
	TranslatePage();

	SendStealthModeCheck();
	SendInstallPluginCheck();
}

function SendStealthModeCheck()
{
	let nVal="no";
	if( $('#StealthMode').is(':checked') )
		nVal="yes";

	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="save_stealth_mode";
	tSend['data']={};
	tSend['data']['action']=nVal;

	SendWXMessage( JSON.stringify(tSend) );

	return true;
}

function SendInstallPluginCheck()
{
	let nVal="no";
	if( $('#InstallCheck').is(':checked') )
		nVal="yes";

	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="network_plugin_install";
	tSend['data']={};
	tSend['data']['action']=nVal;

	SendWXMessage( JSON.stringify(tSend) );

	return true;
}

function FinishGuide()
{
	SendStealthModeCheck();
	SendInstallPluginCheck();

	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="user_guide_finish";
	tSend['data']={};
	tSend['data']['action']="finish";

	SendWXMessage( JSON.stringify(tSend) );
}
