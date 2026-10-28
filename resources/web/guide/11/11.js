// Unlayered 0.1.6: the one-click region page (see index.html). The places come from the slicer
// (privacy_state); the choice is saved there (privacy_save_place) before the privacy page opens.

var m_Place='';
var m_Saving=false;

function OnInit()
{
	TranslatePage();

	SendPrivacy({'command':'privacy_request_state'});
}

function SendPrivacy( tSend )
{
	tSend['sequence_id']=Math.round(new Date() / 1000);
	SendWXMessage( JSON.stringify(tSend) );
}

function HandleStudio( pVal )
{
	let strCmd=pVal['command'];

	if( strCmd=='privacy_state' )
	{
		let list=$('#RegionList');
		list.empty();
		for( let p of pVal['places'] )
		{
			let item=$('<div class="RegionItem" role="radio" aria-checked="false" tabindex="0">');
			item.attr('data-place', p['id']).text(p['label']);
			list.append(item);
		}
		// Only a choice made earlier in this same run of the flow is shown again (after Back).
		if( pVal['place'] )
			ChoosePlace(pVal['place']);
		UpdateState();
	}
	else if( strCmd=='privacy_place_saved' )
	{
		m_Saving=false;
		if( pVal['ok'] )
			window.location.href="../13/index.html";
		else
			UpdateState();
	}
}

function ChoosePlace( id )
{
	m_Place=id;
	$('.RegionItem').removeClass('RegionSelected').attr('aria-checked','false');
	$(".RegionItem[data-place='"+id+"']").addClass('RegionSelected').attr('aria-checked','true');
	UpdateState();
}

function UpdateState()
{
	$('#AcceptBtn').prop('disabled', m_Saving || $('.RegionSelected').length!=1);
}

// Keyboard and mouse both choose (the list is a radio group).
$(document).on('click', '.RegionItem', function() { ChoosePlace($(this).attr('data-place')); });
$(document).on('keydown', '.RegionItem', function(e) {
	if( e.key==' ' || e.key=='Enter' )
	{
		e.preventDefault();
		ChoosePlace($(this).attr('data-place'));
	}
	else if( e.key=='ArrowDown' || e.key=='ArrowUp' || e.key=='ArrowRight' || e.key=='ArrowLeft' )
	{
		e.preventDefault();
		let items=$('.RegionItem');
		let step=( e.key=='ArrowDown' || e.key=='ArrowRight' ) ? 1 : -1;
		let n=items.index(this)+step;
		if( n>=0 && n<items.length )
			items[n].focus();
	}
});

function GotoPolicyPage()
{
	if( m_Place=='' )
		return;
	m_Saving=true;
	UpdateState();
	SendPrivacy({'command':'privacy_save_place','place':m_Place});
}
