/*
    This file is part of Mitsuba, a physically based rendering system.

    Copyright (c) 2007-2011 by Wenzel Jakob and others.

    Mitsuba is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License Version 3
    as published by the Free Software Foundation.

    Mitsuba is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include <mitsuba/render/phase.h>

MTS_NAMESPACE_BEGIN

/*! \plugin{mixturephase}{Mixture phase function}
 *
 * \parameters{
 *     \parameter{weights}{\String}{A comma-separated list of phase function weights}
 *     \parameter{\Unnamed}{\Phase}{Multiple phase function instances that should be
 *     mixed according to the specified weights}
 * }
 *
 * This plugin implements a ``mixture'' scattering model, which represents  
 * linear combinations of multiple phase functions. There is no
 * limit on how many phase function can be mixed, but their combination
 * weights must be non-negative and sum to a value of one or less to ensure
 * energy balance.
 */

class MixturePhase : public PhaseFunction {
public:
	MixturePhase(const Properties &props) 
		: PhaseFunction(props) {
		/* Parse the weight parameter */
		std::vector<std::string> weights = 
			tokenize(props.getString("weights", ""), " ,;");
		if (weights.size() == 0)
			Log(EError, "No weights were supplied!");
		m_weights.resize(weights.size());

		char *end_ptr = NULL;
		for (size_t i=0; i<weights.size(); ++i) {
			Float weight = (Float) strtod(weights[i].c_str(), &end_ptr);
			if (*end_ptr != '\0')
				SLog(EError, "Could not parse the phase function weights!");
			if (weight < 0)
				SLog(EError, "Invalid phase function weight!");
			m_weights[i] = weight;
		}
	}

	MixturePhase(Stream *stream, InstanceManager *manager) 
	 : PhaseFunction(stream, manager) {
		size_t phaseCount = stream->readSize();
		m_weights.resize(phaseCount);
		for (size_t i=0; i<phaseCount; ++i) {
			m_weights[i] = stream->readFloat();
			PhaseFunction *phase = static_cast<PhaseFunction *>(manager->getInstance(stream));
			phase->incRef();
			m_phaseFunctions.push_back(phase);
		}
		configure();
	}

	virtual ~MixturePhase() {
		for (size_t i=0; i<m_phaseFunctions.size(); ++i)
			m_phaseFunctions[i]->decRef();
	}

	void serialize(Stream *stream, InstanceManager *manager) const {
		PhaseFunction::serialize(stream, manager);
		
		stream->writeSize(m_phaseFunctions.size());
		for (size_t i=0; i<m_phaseFunctions.size(); ++i) {
			stream->writeFloat(m_weights[i]);
			manager->serialize(stream, m_phaseFunctions[i]);
		}
	}

	void configure() {
		m_usesRayDifferentials = false;
		size_t componentCount = 0;

		if (m_phaseFunctions.size() != m_weights.size())
			Log(EError, "Phase function count mismatch: " SIZE_T_FMT " phase functions, but specified " SIZE_T_FMT " weights",
				m_phaseFunctions.size(), m_phaseFunctions.size());

		Float totalWeight = 0;
		for (size_t i=0; i<m_weights.size(); ++i)
			totalWeight += m_weights[i];

		if (totalWeight <= 0)
			Log(EError, "The weights must sum to a value greater than zero!");

		if (m_ensureEnergyConservation && totalWeight > 1) {
			std::ostringstream oss;
			Float scale = 1.0f / totalWeight;
			oss << "The phase function " << endl << toString() << endl
				<< "potentially violates energy conservation, since the weights "
				<< "sum to " << totalWeight << ", which is greater than one! "
				<< "They will be re-scaled to avoid potential issues. Specify "
				<< "the parameter ensureEnergyConservation=false to prevent "
				<< "this from happening.";
			Log(EWarn, "%s", oss.str().c_str());
			for (size_t i=0; i<m_weights.size(); ++i)
				m_weights[i] *= scale;
		}

		for (size_t i=0; i<m_phaseFunctions.size(); ++i)
			componentCount += m_phaseFunctions[i]->getComponentCount();

		m_pdf = DiscretePDF(m_phaseFunctions.size());
		m_components.reserve(componentCount);
		m_components.clear();
		m_indices.reserve(componentCount);
		m_indices.clear();
		m_offsets.reserve(m_phaseFunctions.size());
		m_offsets.clear();

		int offset = 0;
		for (size_t i=0; i<m_phaseFunctions.size(); ++i) {
			const PhaseFunction *phase = m_phaseFunctions[i];
			m_offsets.push_back(offset);

			for (int j=0; j<phase->getComponentCount(); ++j) {
				int componentType = phase->getType(j);
				m_components.push_back(componentType);
				m_indices.push_back(std::make_pair((int) i, j));
			}

			offset += phase->getComponentCount();
			m_usesRayDifferentials |= phase->usesRayDifferentials();
			m_pdf[i] = m_weights[i];
		}
		m_pdf.build();
		PhaseFunction::configure();
	}

	Float eval(const PhaseFunctionQueryRecord &pRec) const {
		Spectrum result(0.0f);

		for (size_t i=0; i<m_phaseFunctions.size(); ++i)
			result += m_phaseFunctions[i]->eval(pRec) * m_weights[i];

		return result;
	}

	Float pdf(const PhaseFunctionQueryRecord &pRec) const {
		Float result = 0.0f;

		for (size_t i=0; i<m_phaseFunctions.size(); ++i)
			result += m_phaseFunctions[i]->pdf(pRec) * m_pdf[i];

		return result;
	}

	Spectrum sample(PhaseFunctionQueryRecord &pRec, const Point2 &_sample) const {
		Point2 sample(_sample);
		/* Choose a component based on the normalized weights */
		size_t entry = m_pdf.sampleReuse(sample.x);

		Float pdf;
		Spectrum result = m_phaseFunctions[entry]->sample(pRec, pdf, sample);
		if (result.isZero()) // sampling failed
			return result;

		result *= m_weights[entry] * pdf;
		pdf *= m_pdf[entry];

		for (size_t i=0; i<m_phaseFunctions.size(); ++i) {
			if (entry == i)
				continue;
			pdf += m_phaseFunctions[i]->pdf(pRec) * m_pdf[i];
			result += m_phaseFunctions[i]->eval(pRec) * m_weights[i];
		}

		pRec.sampledComponent += m_offsets[entry];
		return result / pdf;
	}

	Spectrum sample(PhaseFunctionQueryRecord &pRec, Float &pdf, const Point2 &_sample) const {
		Point2 sample(_sample);
		/* Choose a component based on the normalized weights */
		size_t entry = m_pdf.sampleReuse(sample.x);

		Spectrum result = m_phaseFunctions[entry]->sample(pRec, pdf, sample);
		if (result.isZero()) // sampling failed
			return result;

		result *= m_weights[entry] * pdf;
		pdf *= m_pdf[entry];
		
		for (size_t i=0; i<m_phaseFunctions.size(); ++i) {
			if (entry == i)
				continue;
			pdf += m_phaseFunctions[i]->pdf(pRec) * m_pdf[i];
			result += m_phaseFunctions[i]->eval(pRec) * m_weights[i];
		}

		pRec.sampledComponent += m_offsets[entry];
		return result/pdf;
	}

	void addChild(const std::string &name, ConfigurableObject *child) {
		if (child->getClass()->derivesFrom(MTS_CLASS(PhaseFunction))) {
			PhaseFunction *phase = static_cast<PhaseFunction *>(child);
			m_phaseFunctions.push_back(phase);
			phase->incRef();
		} else {
			PhaseFunction::addChild(name, child);
		}
	}

	std::string toString() const {
		std::ostringstream oss;
		oss << "MixturePhase[" << endl
			<< "  weights = {";
		for (size_t i=0; i<m_phaseFunctions.size(); ++i) {
			oss << " " << m_weights[i];
			if (i + 1 < m_phaseFunctions.size())
				oss << ",";
		}
		oss << " }," << endl
			<< "  phaseFunctions = {" << endl;
		for (size_t i=0; i<m_phaseFunctions.size(); ++i) 
			oss << "    " << indent(m_phaseFunctions[i]->toString(), 2) << "," << endl;
		oss << "  }" << endl
			<< "]";
		return oss.str();
	}

	Shader *createShader(Renderer *renderer) const;

	MTS_DECLARE_CLASS()
private:
	std::vector<Float> m_weights;
	std::vector<std::pair<int, int> > m_indices;
	std::vector<int> m_offsets;
	std::vector<PhaseFunction *> m_phaseFunctions;
	DiscretePDF m_pdf;
};

MTS_IMPLEMENT_CLASS_S(MixturePhase, false, PhaseFunction)
MTS_EXPORT_PLUGIN(MixturePhase, "Mixture phase function")
MTS_NAMESPACE_END
