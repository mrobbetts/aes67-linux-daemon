//
//  SinkEdit.jsx
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
import PropTypes from 'prop-types';
import {toast} from 'react-toastify';
import Modal from 'react-modal';

import RestAPI from './Services';
import PcmPicker from './PcmPicker';

const editCustomStyles = {
  content : {
    top:  '50%',
    left: '50%',
    right: 'auto',
    bottom: 'auto',
    marginRight: '-50%',
    transform: 'translate(-50%, -50%)'
  }
};

class SinkEdit extends Component {
  static propTypes = {
    sink: PropTypes.object.isRequired,
    applyEdit: PropTypes.func.isRequired,
    closeEdit: PropTypes.func.isRequired,
    editIsOpen: PropTypes.bool.isRequired,
    isEdit: PropTypes.bool.isRequired,
    editTitle: PropTypes.string.isRequired
  };

  constructor(props) {
    super(props);
    this.state = {
      sources: [],
      id: this.props.sink.id,
      name: this.props.sink.name,
      nameErr: false,
      pcm: this.props.sink.pcm !== undefined ? this.props.sink.pcm : 0,
      pcmChannels: 64,  // the selected pcm's input count; updated by the picker
      io: this.props.sink.io,
      /* canonical delay is TIME (ms). Legacy sinks that predate delay_ms are
         converted from their sample count at the SDP's rate for display. */
      delayMs: this.props.sink.delay_ms > 0
        ? this.props.sink.delay_ms
        : (() => {
            const m = (this.props.sink.sdp || '').match(/L(?:16|24|32)\/(\d+)/);
            return m && this.props.sink.delay
              ? Math.round(this.props.sink.delay * 100000 / parseInt(m[1], 10)) / 100
              : 8;
          })(),
      ignoreRefclkGmid: this.props.sink.ignore_refclk_gmid,
      stream: this.props.sink.stream !== undefined ? this.props.sink.stream : false,
      useSdp: this.props.sink.use_sdp,
      source: this.props.sink.source,
      sourceErr: false,
      sdp: this.props.sink.sdp,
      channels: this.props.sink.map.length,
      map: this.props.sink.map,
      audioMap: []
    }
    let v;
    for (v = 0; v <= (64 - this.state.channels); v += 1) {
      this.state.audioMap.push(v);
    }
    this.onSubmit = this.onSubmit.bind(this);
    this.onCancel = this.onCancel.bind(this);
    this.addSink = this.addSink.bind(this);
    this.onChangeChannels = this.onChangeChannels.bind(this);
    this.onChangeChannelsMap = this.onChangeChannelsMap.bind(this);
    this.onChangePcm = this.onChangePcm.bind(this);
    this.inputIsValid = this.inputIsValid.bind(this);
    this.fetchRemoteSources = this.fetchRemoteSources.bind(this);
    this.onChangeRemoteSourceSDP = this.onChangeRemoteSourceSDP.bind(this);
  }

  fetchRemoteSources() {
    RestAPI.getRemoteSources()
      .then(response => response.json())
      .then(
        data => this.setState( { sources: data.remote_sources }))
  }

  componentDidMount() {
    Modal.setAppElement('body');
    this.fetchRemoteSources();
  }

  addSink(message) {
    RestAPI.addSink(
      this.state.id,
      this.state.name,
      this.state.io,
      this.state.delayMs,
      this.state.useSdp,
      this.state.source ? this.state.source : '',
      this.state.sdp ? this.state.sdp : '',
      this.state.ignoreRefclkGmid,
      this.state.map,
      this.state.pcm,
      this.state.stream,
      this.props.isEdit)
    .then(function(response) {
      this.props.applyEdit();
      toast.success(message);
    }.bind(this));
  }

  onSubmit() {
    this.addSink('Sink ' + this.state.id + (this.props.isEdit ? ' updated ' : ' added'));
  }

  onCancel() {
    this.props.closeEdit();
  }

  // the picker reports the full selected pcm; a sink maps to the card's ALSA
  // INPUT channels, so bound the map by num_inputs. Reset the map if it no
  // longer fits (e.g. switching to a smaller pcm).
  onChangePcm(pcm) {
    const pcmChannels = pcm.num_inputs;
    let channels = this.state.channels;
    let map = this.state.map;
    if (channels > pcmChannels || (map.length > 0 && map[map.length - 1] >= pcmChannels)) {
      channels = Math.max(1, Math.min(channels, pcmChannels));
      map = [];
      for (let v = 0; v < channels; v++) { map.push(v); }
    }
    let audioMap = [];
    for (let v = 0; v <= (pcmChannels - channels); v += 1) { audioMap.push(v); }
    this.setState({ pcm: pcm.pcm_id, pcmChannels: pcmChannels, channels: channels, map: map, audioMap: audioMap });
  }

  onChangeChannels(e) {
    if (e.currentTarget.checkValidity()) {
      let channels = parseInt(e.target.value, 10);
      let audioMap = [], map = [], v;
      for (v = 0; v <= (this.state.pcmChannels - channels); v += 1) {
        audioMap.push(v);
      }
      for (v = 0; v < channels; v++) {
        map.push(v + this.state.map[0]);
      }
      this.setState({ map: map, channels: channels, audioMap: audioMap });
    }
  }

  onChangeChannelsMap(e) {
    let startChannel = parseInt(e.target.value, 10);
    let map = [], v;
    for (v = 0; v < this.state.channels; v++) {
      map.push(v + startChannel);
    }
    this.setState({ map: map });
  }

  onChangeRemoteSourceSDP(e) {
    if (e.target.value) {
      this.setState({ sdp: e.target.value });
    }
  }

  inputIsValid() {
    return !this.state.nameErr &&
      !this.state.sourceErr &&
      (this.state.useSdp || this.state.source) &&
      (!this.state.useSdp || this.state.sdp);
  }

  render()  {
    // a sink can only bind a pcm whose rate matches its SDP rate (the daemon's
    // rate-match guard) and that has enough input channels -- grey out the rest.
    const m = (this.state.sdp || '').match(/a=rtpmap:\d+\s+[A-Za-z0-9]+\/(\d+)/);
    const sdpRate = m ? parseInt(m[1], 10) : 0;
    /* Playout delay options are TIME — the daemon derives samples at each
       attach's rate. Labels show the sample count at THIS SDP's rate, and
       options under one max-size packet (from a=ptime / a=framecount) are
       flagged: the daemon clamps them up (the kernel needs one packet to
       fit inside the delay). */
    const pt = (this.state.sdp || '').match(/a=ptime:([\d.]+)/);
    const fc = (this.state.sdp || '').match(/a=framecount:(?:\d+-)?(\d+)/);
    const pktSamples = pt && sdpRate
      ? Math.ceil(parseFloat(pt[1]) * sdpRate / 1000)
      : (fc ? parseInt(fc[1], 10) : 0);
    const baseDelays = [1, 2, 4, 6, 8, 12, 16, 20];
    const curDelay = parseFloat(this.state.delayMs);
    const delayOptions = (isNaN(curDelay) || baseDelays.includes(curDelay)
      ? baseDelays : baseDelays.concat(curDelay)).sort((a, b) => a - b);
    const delayLabel = (ms) => {
      if (!sdpRate) return ms + ' ms';
      const samples = Math.ceil(ms * sdpRate / 1000);
      return ms + ' ms — ' + samples + ' smp @ ' + (sdpRate / 1000) + ' kHz' +
        (pktSamples && samples < pktSamples ? ' (< 1 packet, clamped up)' : '');
    };
    return (
      <div id='sink-edit'>
        <Modal ariaHideApp={false}
          isOpen={this.props.editIsOpen}
          onRequestClose={this.props.closeEdit}
          style={editCustomStyles}
          contentLabel="Sink Edit">
          <h2><center>{this.props.editTitle}</center></h2>
          <table><tbody>
            <tr>
              <th align="left"> <font color='grey'>ID</font> </th>
              <th align="left"> <input type='number' min='0' max='63' className='input-number' value={this.state.id} onChange={e => this.setState({id: e.target.value})} disabled required/> </th>
            </tr>
            <tr>
              <th align="left"> <label>Name</label> </th>
              <th align="left"> <input value={this.state.name} onChange={e => this.setState({name: e.target.value, nameErr: !e.currentTarget.checkValidity()})} required/> </th>
            </tr>
            <PcmPicker value={this.state.pcm} onChange={this.onChangePcm}
              applicable={(p) => p.num_inputs >= this.state.channels && (!sdpRate || p.sample_rate === sdpRate)} />
            <tr height="35">
              <th align="left"> <label>Use SDP</label> </th>
              <th align="left"> <input type="checkbox" defaultChecked={this.state.useSdp} onChange={e => this.setState({useSdp: e.target.checked})} /> </th>
            </tr>
            <tr>
              <th align="left"> <font color={this.state.useSdp ? 'grey' : 'black'}>Source URL</font> </th>
              <th align="left"> <input type='url' size="30" value={this.state.source} onChange={e => this.setState({source: e.target.value, sourceErr: !e.currentTarget.checkValidity()})} disabled={this.state.useSdp ? true : undefined} required/> </th>
            </tr>
            <tr>
              <th align="left"> <font color={!this.state.useSdp ? 'grey' : 'black'}>Remote Source SDP</font> </th>
              <th align="left">
                <select value={this.state.sdp} onChange={this.onChangeRemoteSourceSDP} disabled={this.state.useSdp ? undefined : true}>
                  <option key='' value=''> -- select a remote source SDP -- </option>
                  {
                    this.state.sources.map((v) => <option key={v.id} value={v.sdp}>{v.source + ' from ' + v.address + ' - ' + v.name}</option>)
		  }
                </select>
              </th>
            </tr>
            <tr>
              <th align="left"> <font color={!this.state.useSdp ? 'grey' : 'black'}>SDP</font> </th>
              <th align="left"> <textarea rows='15' cols='55' value={this.state.sdp} onChange={e => this.setState({sdp: e.target.value})} disabled={this.state.useSdp ? undefined : true} required/> </th>
            </tr>
            <tr>
              <th align="left"> <label>Playout delay</label> </th>
              <th align="left">
	        <select value={this.state.delayMs} onChange={e => this.setState({delayMs: e.target.value})}>
                  {delayOptions.map((ms) =>
                    <option key={ms} value={ms}>{delayLabel(ms)}</option>)}
                </select>
              </th>
            </tr>
            <tr height="35">
              <th align="left"> <label>Ignore RefClk GMID</label> </th>
              <th align="left"> <input type="checkbox" defaultChecked={this.state.ignoreRefclkGmid} onChange={e => this.setState({ignoreRefclkGmid: e.target.checked})} /> </th>
            </tr>
            <tr height="35">
              <th align="left"> <label>HTTP stream</label> </th>
              <th align="left"> <input type="checkbox" defaultChecked={this.state.stream} onChange={e => this.setState({stream: e.target.checked})} /> </th>
            </tr>
            <tr>
              <th align="left"> <label>Channels</label> </th>
              <th align="left"> <input type='number' min='1' max={this.state.pcmChannels} className='input-number' value={this.state.channels} onChange={this.onChangeChannels} required/> </th>
            </tr>
            <tr>
              <th align="left">Audio Channels map</th>
              <th align="left">
                <select value={this.state.map[0]} onChange={this.onChangeChannelsMap}>
                  { this.state.channels > 1 ?
                      this.state.audioMap.map((v) => <option key={v} value={v}>{'ALSA Input ' + parseInt(v + 1, 10) + ' -> ALSA Input ' +  parseInt(v + this.state.channels, 10)}</option>) :
                      this.state.audioMap.map((v) => <option key={v} value={v}>{'ALSA Input ' + parseInt(v + 1, 10)}</option>)
		  }
                </select>
              </th>
            </tr>
          </tbody></table>
          <br/>
	  <div style={{textAlign: 'center'}}>
            <button onClick={this.onSubmit} disabled={this.inputIsValid() ? undefined : true}>Submit</button>
            &nbsp;&nbsp;&nbsp;&nbsp;
            <button onClick={this.onCancel}>Cancel</button>
          </div>
        </Modal>
      </div>
    );
  }
}

export default SinkEdit;
