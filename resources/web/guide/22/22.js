function OnInit()
{
	TranslatePage();
	
	RequestProfile();
}

function HandleStudio(pVal)
{
	let strCmd=pVal['command'];
	//alert(strCmd);
	
	if(strCmd=='response_userguide_profile')
	{
		m_ProfileItem=pVal['response'];
		SortUI();
	}
}

function ReturnPreviewPage()
{
	/* ORCA the profile reply is held until the filament data is loaded, so this can fire before
	   m_ProfileItem exists. Fall back to the printer picker rather than throwing - page 22 is the
	   last page of the wizard and the first-run dialog has no close box. */
	if(!m_ProfileItem)
	{
		document.location.href="../21/index.html";
		return;
	}

	let nMode=m_ProfileItem["model"].length;
	
	if( nMode==1)
		document.location.href="../1/index.html";
	else
		document.location.href="../21/index.html";	
}

function FinishGuide()
{
	/* ORCA the filament selection still has to be saved here -- ResponseFilamentResult() posts it and
	   refuses to advance when nothing is ticked. The wizard now ends on page 25 (the multiplanar
	   tutorial offer), which is what sends user_guide_finish. */
	let bRet=ResponseFilamentResult();

	if(bRet)
		window.location.href="../25/index.html";
}
