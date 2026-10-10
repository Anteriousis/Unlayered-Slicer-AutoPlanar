// Final wizard step: offer the bundled multiplanar walkthrough, and say where to find it later.
// Skip and Finish do the same thing -- the tutorial opens in the default browser, so the wizard
// does not need to wait for it and there is nothing to confirm.

function OnInit()
{
	TranslatePage();
}

function OpenTutorial()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="open_multiplanar_tutorial";

	SendWXMessage( JSON.stringify(tSend) );
}

function ReturnFilamentPage()
{
	document.location.href="../22/index.html";
}

// Unlayered 2026-10-09: no longer the last page. Finish/Skip go on to the one-page "Privacy and
// printer connections" question (stealth mode + Bambu Lab network plug-in), which finishes the guide.
function FinishGuide()
{
	window.location.href="../4unlayered/index.html";
}
