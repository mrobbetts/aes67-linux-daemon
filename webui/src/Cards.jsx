//
//  Cards.jsx
//
//  Copyright (c) 2019 2020 Andrea Bondavalli. All rights reserved.
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//

import React, {Component} from 'react';

import RestAPI from './Services';
import Loader from './Loader';
import CardEdit from './CardEdit';
import PcmEdit from './PcmEdit';

//
// W10.2 runtime cards. The Cards tab renders the card -> pcm -> stream tree:
// cards and pcms are managed here (add/remove card, add/edit/remove pcm), and
// the streams bound to each pcm appear as read-only leaves (edited on the
// Sources/Sinks tabs). The tree is joined client-side from the flat REST lists.
//
class Cards extends Component {
  constructor(props) {
    super(props);
    this.state = {
      cards: [],
      pcms: [],
      sources: [],
      sinks: [],
      ptpDomains: [],
      pcmClocks: [],
      isLoading: false,
      cardEditIsOpen: false,
      isCardEdit: false,
      cardEditCard: null,
      pcmEditIsOpen: false,
      pcmEditCard: '',
      pcmEditTitle: '',
      pcm: {},
      isPcmEdit: false
    };
    this.fetchAll = this.fetchAll.bind(this);
    this.applyEdit = this.applyEdit.bind(this);
    this.closeCardEdit = this.closeCardEdit.bind(this);
    this.closePcmEdit = this.closePcmEdit.bind(this);
    this.onAddCard = this.onAddCard.bind(this);
    this.onEditCard = this.onEditCard.bind(this);
    this.onRemoveCard = this.onRemoveCard.bind(this);
    this.onAddPcm = this.onAddPcm.bind(this);
    this.onEditPcm = this.onEditPcm.bind(this);
    this.onRemovePcm = this.onRemovePcm.bind(this);
    this.streamsForPcm = this.streamsForPcm.bind(this);
  }

  fetchAll() {
    this.setState({isLoading: true});
    Promise.all([
      RestAPI.getCards().then(r => r.json()),
      RestAPI.getAllPcms().then(r => r.json()),
      RestAPI.getSources().then(r => r.json()),
      RestAPI.getSinks().then(r => r.json())
    ]).then(([cards, pcms, sources, sinks]) => {
      this.setState({
        cards: cards.cards || [],
        pcms: pcms.pcms || [],
        sources: sources.sources || [],
        sinks: sinks.sinks || [],
        isLoading: false
      });
    }).catch(() => this.setState({isLoading: false}));
    this.fetchClocks();
  }

  // lightweight live refresh (no isLoading flicker): the rate-bearing card/pcm
  // state + the clock badges. Sources/sinks change on user action / the manual
  // refresh button, so they stay on fetchAll. Runs on the poll interval so the
  // Cards view reflects e.g. an auto-follow re-rate (and W28 live/armed rate)
  // without a tab switch.
  refreshLive() {
    Promise.all([
      RestAPI.getCards().then(r => r.json()),
      RestAPI.getAllPcms().then(r => r.json())
    ]).then(([cards, pcms]) => this.setState({
      cards: cards.cards || [],
      pcms: pcms.pcms || []
    })).catch(() => {});
    this.fetchClocks();
  }

  // poll the live clock state for the badges: per-domain (card) + per-PCM (TIC).
  fetchClocks() {
    Promise.all([
      RestAPI.getPTPDomains().then(r => r.json()),
      RestAPI.getPcmClocks().then(r => r.json())
    ]).then(([domains, pcms]) => this.setState({
      ptpDomains: domains.domains || [],
      pcmClocks: pcms.pcms || []
    })).catch(() => {});
  }

  // a coloured dot for a clock status string (green locked / amber locking /
  // red unlocked or unknown).
  statusDot(status, title) {
    const color = status === 'locked' ? '#2a0'
                : status === 'locking' ? '#d90' : '#c00';
    return (<span title={title + ': ' + status}
              style={{color: color}}>&#9679;</span>);
  }

  // a card's PTP domain lock; pairs with the full Clocks view in the PTP tab.
  lockDot(domain) {
    const d = this.state.ptpDomains.find(x => x.domain === domain);
    return this.statusDot(d ? d.status : 'unlocked', 'PTP domain ' + domain);
  }

  // a PCM's TIC engine lock — is this media clock actually tracking?
  pcmLockDot(pcmId) {
    const p = this.state.pcmClocks.find(x => x.pcm_id === pcmId);
    return this.statusDot(p ? p.tic_status : 'unlocked',
                          'TIC engine (pcm ' + pcmId + ')');
  }

  // W28: the PCM's rate from kernel truth — the live rate (what the chip is keyed
  // to now), a note if the configured/intent rate differs, and an amber
  // armed->target marker when an in-place re-rate is latched but not yet applied.
  pcmRate(p) {
    const c = this.state.pcmClocks.find(x => x.pcm_id === p.pcm_id);
    const live = (c && c.live_rate) ? c.live_rate : p.sample_rate;
    const pending = c ? c.pending_rate : 0;
    return (<>
      {live} Hz
      {live !== p.sample_rate &&
        <span title='configured rate (intent); the chip is live at the rate shown'>
          {' '}(cfg {p.sample_rate})</span>}
      {pending > 0 &&
        <span style={{color: '#d90'}}
          title={'armed: in-place re-rate to ' + pending +
                 ' Hz pending — applies when the client releases the device'}>
          {' '}&rarr; {pending} Hz &#10227;</span>}
    </>);
  }

  componentDidMount() {
    this.fetchAll();
    this.liveInterval = setInterval(() => { this.refreshLive() }, 3000);
  }

  componentWillUnmount() {
    clearInterval(this.liveInterval);
  }

  applyEdit() {
    this.setState({cardEditIsOpen: false, pcmEditIsOpen: false});
    this.fetchAll();
  }

  closeCardEdit() {
    this.setState({cardEditIsOpen: false});
    this.fetchAll();
  }

  closePcmEdit() {
    this.setState({pcmEditIsOpen: false});
    this.fetchAll();
  }

  onAddCard() {
    this.setState({cardEditIsOpen: true, isCardEdit: false, cardEditCard: null});
  }

  onEditCard(card) {
    this.setState({cardEditIsOpen: true, isCardEdit: true, cardEditCard: card});
  }

  onRemoveCard(name) {
    if (!window.confirm('Remove card "' + name + '" and all its PCMs and bound streams?')) {
      return;
    }
    RestAPI.removeCard(name).then(() => this.fetchAll());
  }

  onAddPcm(cardName) {
    const defaultPcm = {
      name: '', sample_rate: 48000, num_inputs: 2, num_outputs: 2,
      playout_delay: 0, capture_delay: 0, rate_follows_source: false,
      rate_follow_strict_version: false
    };
    this.setState({
      pcmEditIsOpen: true, pcmEditCard: cardName, pcm: defaultPcm,
      isPcmEdit: false, pcmEditTitle: 'Add PCM to ' + cardName
    });
  }

  onEditPcm(cardName, pcm) {
    this.setState({
      pcmEditIsOpen: true, pcmEditCard: cardName, pcm: pcm,
      isPcmEdit: true, pcmEditTitle: 'Edit PCM "' + pcm.name + '" on ' + cardName
    });
  }

  onRemovePcm(cardName, pcmName) {
    if (!window.confirm('Remove PCM "' + pcmName + '" from "' + cardName +
        '"? This recreates the card (brief audio interruption) and drops streams bound to it.')) {
      return;
    }
    RestAPI.removePcm(cardName, pcmName).then(() => this.fetchAll());
  }

  streamsForPcm(pcmId) {
    const srcs = this.state.sources.filter(x => x.pcm === pcmId)
      .map(x => ({key: 'src' + x.id, type: 'source', name: x.name}));
    const snks = this.state.sinks.filter(x => x.pcm === pcmId)
      .map(x => ({key: 'snk' + x.id, type: 'sink', name: x.name}));
    return srcs.concat(snks);
  }

  render() {
    if (this.state.isLoading) {
      return <Loader/>;
    }
    return (
      <div id='cards'>
        <div className='cards-tree'>
          { this.state.cards.length === 0 ?
            <p>No cards configured</p> :
            this.state.cards.map(card => {
              const cardPcms = this.state.pcms.filter(p => p.card === card.name);
              return (
                <div className='tree-card' key={card.name}
                  style={{border: '1px solid #ccc', borderRadius: '6px', margin: '8px 0', padding: '8px'}}>
                  <div className='tree-card-head'>
                    <b>{card.name}</b> &nbsp;<font color='grey'>(domain {card.domain})</font>&nbsp;{this.lockDot(card.domain)}
                    { card.rate_change_mode === 'in-place' ?
                      <font color='#0a7' title='re-rate in place (no card rebuild)'> &nbsp;· in-place</font> : undefined }
                    &nbsp;&nbsp;
                    <span className='pointer-area' title='Add PCM' onClick={() => this.onAddPcm(card.name)}>
                      <img width='20' height='20' src='/plus.png' alt='+pcm'/> </span>
                    &nbsp;
                    <span className='pointer-area' title='Edit card (rename / domain / rate-change mode)' onClick={() => this.onEditCard(card)}>
                      <img width='20' height='20' src='/edit.png' alt='edit'/> </span>
                    &nbsp;
                    <span className='pointer-area' title='Remove card' onClick={() => this.onRemoveCard(card.name)}>
                      <img width='20' height='20' src='/trash.png' alt='x'/> </span>
                  </div>
                  { cardPcms.length === 0 ?
                    <div style={{marginLeft: '20px', color: 'grey'}}>(no PCMs — add one)</div> :
                    cardPcms.map(p => {
                      const streams = this.streamsForPcm(p.pcm_id);
                      return (
                        <div className='tree-pcm' key={p.pcm_id} style={{marginLeft: '20px', marginTop: '6px'}}>
                          <div>
                            ▸ <b>{p.name}</b> &nbsp;{this.pcmLockDot(p.pcm_id)} &nbsp;
                            <font color='grey'>
                              {this.pcmRate(p)} · {p.num_inputs} in / {p.num_outputs} out · hw:{card.name},{p.device_index}
                            </font>
                            &nbsp;&nbsp;
                            <span className='pointer-area' title='Edit PCM' onClick={() => this.onEditPcm(card.name, p)}>
                              <img width='18' height='18' src='/edit.png' alt='edit'/> </span>
                            &nbsp;
                            <span className='pointer-area' title='Remove PCM' onClick={() => this.onRemovePcm(card.name, p.name)}>
                              <img width='18' height='18' src='/trash.png' alt='x'/> </span>
                          </div>
                          { streams.length === 0 ?
                            <div style={{marginLeft: '24px', color: '#aaa'}}>· no streams bound</div> :
                            streams.map(st => (
                              <div key={st.key} style={{marginLeft: '24px'}}>
                                · <font color='grey'>{st.type}</font> &nbsp;{st.name}
                              </div>
                            ))
                          }
                        </div>
                      );
                    })
                  }
                </div>
              );
            })
          }
        </div>
        &nbsp;
        <span className='pointer-area' title='Reload' onClick={this.fetchAll}>
          <img width='30' height='30' src='/reload.png' alt='reload'/> </span>
        &nbsp;&nbsp;
        <span className='pointer-area' title='Add card' onClick={this.onAddCard}>
          <img width='30' height='30' src='/plus.png' alt='add'/> </span>

        { this.state.cardEditIsOpen ?
          <CardEdit editIsOpen={this.state.cardEditIsOpen}
            closeEdit={this.closeCardEdit}
            applyEdit={this.applyEdit}
            isEdit={this.state.isCardEdit}
            card={this.state.cardEditCard} />
          : undefined }
        { this.state.pcmEditIsOpen ?
          <PcmEdit editIsOpen={this.state.pcmEditIsOpen}
            closeEdit={this.closePcmEdit}
            applyEdit={this.applyEdit}
            cardName={this.state.pcmEditCard}
            editTitle={this.state.pcmEditTitle}
            isEdit={this.state.isPcmEdit}
            pcm={this.state.pcm} />
          : undefined }
      </div>
    );
  }
}

export default Cards;
