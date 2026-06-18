//
//  PcmEdit.jsx
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

const editCustomStyles = {
  content : {
    top: '50%', left: '50%', right: 'auto', bottom: 'auto',
    marginRight: '-50%', transform: 'translate(-50%, -50%)'
  }
};

// Add / edit a PCM on a card. The name is the (card-scoped) identity, so it is
// read-only on edit. Any change recreates the owning card (recreate-card), with
// the brief audio interruption that implies.
class PcmEdit extends Component {
  static propTypes = {
    cardName: PropTypes.string.isRequired,
    pcm: PropTypes.object.isRequired,
    applyEdit: PropTypes.func.isRequired,
    closeEdit: PropTypes.func.isRequired,
    editIsOpen: PropTypes.bool.isRequired,
    isEdit: PropTypes.bool.isRequired,
    editTitle: PropTypes.string.isRequired
  };

  constructor(props) {
    super(props);
    this.state = {
      name: this.props.pcm.name || '',
      nameErr: !(this.props.pcm.name && this.props.pcm.name.length > 0),
      sampleRate: this.props.pcm.sample_rate || 48000,
      numInputs: this.props.pcm.num_inputs || 0,
      numOutputs: this.props.pcm.num_outputs || 0,
      playoutDelay: this.props.pcm.playout_delay || 0,
      captureDelay: this.props.pcm.capture_delay || 0
    };
    this.onSubmit = this.onSubmit.bind(this);
    this.onCancel = this.onCancel.bind(this);
  }

  componentDidMount() {
    Modal.setAppElement('body');
  }

  onSubmit() {
    const done = function() {
      toast.success('PCM ' + this.state.name + (this.props.isEdit ? ' updated' : ' added'));
      this.props.applyEdit();
    }.bind(this);
    if (this.props.isEdit) {
      // URL carries the current (old) name; the body carries the possibly-new one.
      RestAPI.updatePcm(this.props.cardName, this.props.pcm.name, this.state.name,
        this.state.sampleRate, this.state.numInputs, this.state.numOutputs,
        this.state.playoutDelay, this.state.captureDelay).then(done);
    } else {
      RestAPI.addPcm(this.props.cardName, this.state.name, this.state.sampleRate,
        this.state.numInputs, this.state.numOutputs, this.state.playoutDelay,
        this.state.captureDelay).then(done);
    }
  }

  onCancel() {
    this.props.closeEdit();
  }

  render() {
    return (
      <div id='pcm-edit'>
        <Modal ariaHideApp={false}
          isOpen={this.props.editIsOpen}
          onRequestClose={this.props.closeEdit}
          style={editCustomStyles}
          contentLabel='PCM Edit'>
          <h2><center>{this.props.editTitle}</center></h2>
          { this.props.isEdit ?
            <p style={{textAlign: 'center', color: '#a60'}}>
              Editing recreates the card — a brief audio interruption, and any stream
              that no longer fits (e.g. a sink whose rate no longer matches) is dropped.
            </p> : undefined }
          <table><tbody>
            <tr>
              <th align='left'> <label>Name</label> </th>
              <th align='left'> <input value={this.state.name}
                onChange={e => this.setState({name: e.target.value, nameErr: e.target.value.length === 0})}
                required/> </th>
            </tr>
            <tr>
              <th align='left'> <label>Sample rate</label> </th>
              <th align='left'>
                <select value={this.state.sampleRate} onChange={e => this.setState({sampleRate: e.target.value})}>
                  <option value='44100'>44100</option>
                  <option value='48000'>48000</option>
                  <option value='88200'>88200</option>
                  <option value='96000'>96000</option>
                  <option value='176400'>176400</option>
                  <option value='192000'>192000</option>
                </select>
              </th>
            </tr>
            <tr>
              <th align='left'> <label>Inputs (capture ch)</label> </th>
              <th align='left'> <input type='number' min='0' max='64' className='input-number'
                value={this.state.numInputs}
                onChange={e => this.setState({numInputs: e.target.value})}/> </th>
            </tr>
            <tr>
              <th align='left'> <label>Outputs (playback ch)</label> </th>
              <th align='left'> <input type='number' min='0' max='64' className='input-number'
                value={this.state.numOutputs}
                onChange={e => this.setState({numOutputs: e.target.value})}/> </th>
            </tr>
          </tbody></table>
          <br/>
          <div style={{textAlign: 'center'}}>
            <button onClick={this.onSubmit} disabled={this.state.nameErr ? true : undefined}>Submit</button>
            &nbsp;&nbsp;&nbsp;&nbsp;
            <button onClick={this.onCancel}>Cancel</button>
          </div>
        </Modal>
      </div>
    );
  }
}

export default PcmEdit;
