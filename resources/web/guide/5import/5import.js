// Unlayered 0.1.7: the "Settings from other slicers" step (see index.html).

var m_Busy=false;
var m_Importing='';

function OnInit()
{
	TranslatePage();

	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="request_other_slicers";
	SendWXMessage( JSON.stringify(tSend) );
}

function HandleStudio( pVal )
{
	let strCmd=pVal['command'];

	if( strCmd=='page_before_import' )
	{
		window.location.href="../"+(pVal['page']=='13signin' ? '13signin' : '13')+"/index.html";
		return;
	}

	if( strCmd=='other_slicers' )
	{
		$('#ImportTitle').text(pVal['title']);
		$('#ImportText').text(pVal['text']);
		$('#ImportNote').text(pVal['note']);
		$('#ImportBtn').text(pVal['import']);
		m_Importing=pVal['importing'];

		if( pVal['imported'] )
		{
			// Back from the printer picker after importing: show the result again.
			ShowSummary(pVal['summary']);
			return;
		}

		let list=$('#AppList');
		list.empty();
		let apps=pVal['apps'] || [];
		for( let n=0; n<apps.length; n++ )
		{
			let label=$('<label class="CheckArea"></label>');
			let box=$('<input type="checkbox" checked>').attr('data-index', apps[n]['index']).on('change', UpdateButtons);
			label.append(box).append($('<a class="TextS1"></a>').text(apps[n]['label']));
			list.append(label);
		}
		// Nothing left to import (should not happen: the step is only shown when there is): go on.
		if( apps.length==0 )
			GoNext();
		UpdateButtons();
	}
	else if( strCmd=='other_slicers_imported' )
	{
		m_Busy=false;
		ShowSummary(pVal['summary']);
	}
}

function SelectedApps()
{
	let out=[];
	$('#AppList input[type=checkbox]:checked').each(function(){ out.push(parseInt($(this).attr('data-index'))); });
	return out;
}

function UpdateButtons()
{
	let off=m_Busy || SelectedApps().length==0;
	$('#ImportBtn').toggleClass('ButtonStyleDisabled', off).toggleClass('ButtonStyleConfirm', !off);
}

function DoImport()
{
	let apps=SelectedApps();
	if( m_Busy || apps.length==0 )
		return;
	m_Busy=true;
	$('#AppList input').prop('disabled', true);
	if( m_Importing!='' )
		$('#ImportBtn').text(m_Importing);
	$('#SkipBtn').prop('hidden', true);
	UpdateButtons();

	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="import_other_slicers";
	tSend['apps']=apps;
	SendWXMessage( JSON.stringify(tSend) );
}

// What came over and what did not, per app; Next goes on to the printer picker.
function ShowSummary( text )
{
	$('#AppList').prop('hidden', true);
	$('#ImportNote').prop('hidden', true);
	$('#ImportText').prop('hidden', true);
	$('#Summary').text(text).prop('hidden', false);
	$('#ImportBtn').prop('hidden', true);
	$('#SkipBtn').prop('hidden', true);
	$('#NextBtn').prop('hidden', false);
}

function GoBack()
{
	if( m_Busy )
		return;
	// Unlayered 0.1.7: back to the sign-in step when the wizard showed it, else the privacy page.
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="request_page_before_import";
	SendWXMessage( JSON.stringify(tSend) );
	setTimeout(function(){ window.location.href="../13/index.html"; }, 15000);
}

function GoNext()
{
	if( m_Busy )
		return;
	window.location.href="../21/index.html";
}
