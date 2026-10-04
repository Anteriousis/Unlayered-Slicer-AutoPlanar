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

function FinishGuide()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="user_guide_finish";
	tSend['data']={};
	tSend['data']['action']="finish";

	SendWXMessage( JSON.stringify(tSend) );
}
