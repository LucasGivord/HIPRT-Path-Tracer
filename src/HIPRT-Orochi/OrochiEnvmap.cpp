/*
 * Copyright 2024 Tom Clabault. GNU GPL3 license.
 * GNU GPL3 license copy: https://www.gnu.org/licenses/gpl-3.0.txt
 */

#include "HIPRT-Orochi/OrochiEnvmap.h"

OrochiEnvmap::OrochiEnvmap(Image32Bit& image) : OrochiTexture(image)
{
	compute_cdf(image);
}

OrochiEnvmap::OrochiEnvmap(OrochiEnvmap&& other) : OrochiTexture(std::move(other))
{
	m_cdf = std::move(other.m_cdf);
}

void OrochiEnvmap::operator=(OrochiEnvmap&& other)
{
	OrochiTexture::operator=(std::move(other));

	m_cdf = std::move(other.m_cdf);
}

void OrochiEnvmap::init_from_image(const Image32Bit& image)
{
	OrochiTexture::init_from_image(image);
}

void OrochiEnvmap::compute_cdf(Image32Bit& image)
{
	std::vector<float> cdf = image.compute_get_cdf();

	m_cdf.resize(width * height);
	m_cdf.upload_data(cdf.data());
}

OrochiBuffer<float>& OrochiEnvmap::get_cdf_buffer()
{
	return m_cdf;
}

float* OrochiEnvmap::get_cdf_device_pointer()
{
	if (m_cdf.get_element_count() == 0)
		std::cerr << "Trying to get the CDF of an OrochiEnvmap whose CDF wasn't computed in the first place..." << std::endl;

	return m_cdf.get_device_pointer();
}
