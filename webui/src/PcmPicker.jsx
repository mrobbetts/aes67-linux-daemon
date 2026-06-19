//
//  PcmPicker.jsx
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

import RestAPI from './Services';

//
// Cascading Card -> PCM picker. Controlled by the parent via `value` (the
// internal pcm_id) and `onChange(pcm_id)`. The user thinks in (card, pcm-name);
// the picker resolves that to the pcm_id the daemon binds streams by. Renders
// two <tr> rows for embedding directly inside an edit form's <tbody>.
//
// onChange receives the full selected pcm object (so the parent can bound its
// channel map by num_inputs/num_outputs), not just the pcm_id.
class PcmPicker extends Component {
  static propTypes = {
    value: PropTypes.number,        // current pcm_id
    onChange: PropTypes.func.isRequired,
    applicable: PropTypes.func      // optional (pcm)=>bool; inapplicable = greyed
  };

  constructor(props) {
    super(props);
    this.state = { cards: [], pcms: [] };
    this.onChangePcm = this.onChangePcm.bind(this);
  }

  componentDidMount() {
    Promise.all([
      RestAPI.getCards().then(r => r.json()),
      RestAPI.getAllPcms().then(r => r.json())
    ]).then(([c, p]) => {
      const pcms = p.pcms || [];
      this.setState({ cards: c.cards || [], pcms: pcms });
      // always report the resolved pcm on load (snapping to the first one if the
      // current value is stale) so the parent learns its channel counts.
      const pcm = pcms.find(x => x.pcm_id === this.props.value) || pcms[0];
      if (pcm) {
        this.props.onChange(pcm);
      }
    }).catch(() => {});
  }

  onChangePcm(e) {
    const pcm = this.state.pcms.find(x => x.pcm_id === parseInt(e.target.value, 10));
    if (pcm) {
      this.props.onChange(pcm);
    }
  }

  render() {
    // one dropdown, grouped by card; inapplicable pcms greyed (disabled).
    const applicable = this.props.applicable || (() => true);
    return (
      <tr>
        <th align='left'> <label>Card / PCM</label> </th>
        <th align='left'>
          <select value={this.props.value} onChange={this.onChangePcm}>
            { this.state.cards.map(c => {
              const cardPcms = this.state.pcms.filter(x => x.card === c.name);
              return (
                <optgroup key={c.name} label={c.name + ' (domain ' + c.domain + ')'}>
                  { cardPcms.map(p => {
                    const ok = applicable(p);
                    return (
                      <option key={p.pcm_id} value={p.pcm_id} disabled={!ok}>
                        {p.name + ' — ' + p.sample_rate + ' Hz, ' + p.num_inputs + 'in/' + p.num_outputs + 'out, hw:' + p.card + ',' + p.device_index + (ok ? '' : '  (n/a)')}
                      </option>
                    );
                  }) }
                </optgroup>
              );
            }) }
          </select>
        </th>
      </tr>
    );
  }
}

export default PcmPicker;
