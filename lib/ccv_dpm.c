#include "ccv.h"
#include "ccv_internal.h"
#include <sys/time.h>
#ifdef HAVE_GSL
#include <gsl/gsl_rng.h>
#include <gsl/gsl_multifit.h>
#include <gsl/gsl_randist.h>
#endif
#ifdef USE_OPENMP
#include <omp.h>
#endif
#ifdef HAVE_LIBLINEAR
#include <linear.h>
#endif

const ccv_dpm_param_t ccv_dpm_default_params = 
{
	.interval = 8,
	.min_neighbors = 1,
	.flags = 0,
	.threshold = 0.6, // 0.8
};

#define CCV_DPM_WINDOW_SIZE (8)

FILE* g_pFile;
char* g_pcLog = "this is cyg";
int g_iTemp = 12345;


LogStart()
{
    //int i = 0;
    g_pFile = fopen("log.txt", "w");
    fprintf(g_pFile, "%s", g_pcLog);
}

LogProcess()
{
    //fprintf(g_pFile, "%d\n", g_iTemp);
    fprintf(g_pFile, "%s\n", g_pcLog);

}

LogEnd()
{
    fclose(g_pFile);

}

LogTest()
{
    FILE *fFile;
    fFile = fopen("log1.txt", "w");
    //fprintf(fFile, "%s", g_pcLog);
    fputs("this is cyg", fFile);
    fclose(fFile);

}


static int 
_ccv_dpm_scale_upto(ccv_dense_matrix_t* a, 
					ccv_dpm_mixture_model_t** _model, 
					int count, 
					int interval)
{
	int c, i;
	ccv_size_t size = ccv_size(a->cols, a->rows);

	// count = 1
	for (c = 0; c < count; c++)
	{
		ccv_dpm_mixture_model_t* model = _model[c];

		for (i = 0; i < model->count; i++)
		{
			size.width = ccv_min(model->root[i].root.w->cols * CCV_DPM_WINDOW_SIZE, size.width);
			size.height = ccv_min(model->root[i].root.w->rows * CCV_DPM_WINDOW_SIZE, size.height);
		}
	}

	// 计算分块后的图像高和宽
	int hr = a->rows / size.height;
	int wr = a->cols / size.width;
	double scale = pow(2.0, 1.0 / (interval + 1.0));
	int next = interval + 1;
	
	return (int)(log((double)ccv_min(hr, wr)) / log(scale)) - next;
}

/*
    我们的所有模型都涉及将线性滤波器应用到稠密特征映射中。特征映射(feature map)
是一个数组，其元素由从一张图像中计算出来的所有d维特征向量组成，每个特征向量描述
一块图像区域。实际中我们用的是论文[10]中的HOG特征的变体，但这里讨论的框架是与特
征选择无关的。
    滤波器(filter)是由d维权重向量定义的一个矩形模版。滤波器F在特征映射G中位置
(x,y)处的响应值(response)或得分是滤波器向量与以(x, y)为左上角点的子窗口的特征向
量的点积(DotProduct)：

    我们想要定义在图像不同位置和尺度的得分，这是通过使用特征金字塔来实现的，特
征金字塔表示了一定范围内有限几个尺度构成的特征映射。首先通过不断的平滑和子采样
计算一个标准的图像金字塔，然后计算金字塔中每层图像的所有特征。图3展示了特征金字塔。

	图3，特征金字塔和其中的一个人体模型实例。部件滤波器位于根滤波器两倍空间分辨
率的金字塔层。
 
	根据参数λ在特征金字塔中进行尺度采样，λ定义了组中层的个数。也就是说，λ是
我们为了获得某一层的两倍分辨率而需要在金字塔中向下走的层数。实际中在训练时λ=5，
在测试时λ=10。尺度空间的精细采样对于我们的方法获得较高的性能表现非常重要。
    [10]中的系统使用单个滤波器来定义整个目标模型，它计算滤波器在HOG特征金字塔中
所有位置和层的得分，通过对得分阈值化来检测目标。
    假设F是一个w * h 大小的滤波器，H是特征金字塔，p = (x, y, l)指定金字塔中l层
的位置(x, y)，φ(H,p, w, h)表示金字塔H中以p为左上角点的w*h大小的子窗口中的所有
特征向量按行优先顺序串接起来得到的向量。滤波器F在位置p处的得分为F’·φ(H, p, w, h)，
F’表示将F中的权重向量按行优先顺序串接起来得到的向量。此后我们使用F’·φ(H,p)
代替F’·φ(H, p, w, h)，因为子窗口的大小已隐含包括在滤波器F中。
*/
static void _ccv_dpm_feature_pyramid(ccv_dense_matrix_t* a, 
									 ccv_dense_matrix_t** pyr, 
									 int scale_upto, 
									 int interval)
{
	// λ是我们为了获得某一层的两倍分辨率而需要在金字塔中向下走的层数
	// .interval = 8, next == λ == 9
	int next = interval + 1;
	double scale = pow(2.0, 1.0 / (interval + 1.0));
	memset(pyr, 0, (scale_upto + next * 2) * sizeof(ccv_dense_matrix_t*));
	pyr[next] = a;
	int i;
	
	for (i = 1; i <= interval; i++)
		ccv_resample(pyr[next], &pyr[next + i], 0, (int)(pyr[next]->rows / pow(scale, i)), (int)(pyr[next]->cols / pow(scale, i)), CCV_INTER_AREA);

	for (i = next; i < scale_upto + next; i++)
		ccv_sample_down(pyr[i], &pyr[i + next], 0, 0, 0);

	ccv_dense_matrix_t* hog;

	// 一个产生上一级HOG的更有效的方法(用更小的尺寸)
	/* a more efficient way to generate up-scaled hog (using smaller size) */
	for (i = 0; i < next; i++)
	{
		hog = 0;
		ccv_hog(pyr[i + next], &hog, 0, 9, CCV_DPM_WINDOW_SIZE / 2 /* this is */);
		pyr[i] = hog;
	}
	
	hog = 0;
	ccv_hog(pyr[next], &hog, 0, 9, CCV_DPM_WINDOW_SIZE);
	pyr[next] = hog;
	
	for (i = next + 1; i < scale_upto + next * 2; i++)
	{
		hog = 0;
		ccv_hog(pyr[i], &hog, 0, 9, CCV_DPM_WINDOW_SIZE);
		ccv_matrix_free(pyr[i]);
		pyr[i] = hog;
	}
}

/* 
score(x0, y0, l0) = R0l0(x0, y0) + Sigma(1~n){(Di,(l0-lambda))(2(x0, y0) + vi) + b} 

R0l0(x0, y0)是rootfilter(主模型)的得分,或者说是匹配程度,本质就是Beta和Feature的
卷积，后面的partfilter也是如此。中间是n个partfilter（子模型）的得分。b是为了
component之间对齐而设的rootoffset. (x0, y0)为rootfilter的left-top位置在
root feature map中的坐标，2(x0, y0) + vi)为第i个partfilter映射到part feature map
中的坐标。*2是因为part feature map的分辨率是root feature map的两倍，vi为相对于
rootfilter left-top的偏移。

PartFilter_i的得分如下：
(Di,l)(x, y) =  max ((Ri,l)(x + dx, y + dy) - di dp Phi_d(dx, dy))
			   dx,dy
上式是在patfilter理想位置(x, y),即anchor position的一定范围内，寻找一个综合匹配
和形变最优的位置。(dx, dy)为偏移向量，di为偏移向量(dx, dy, dxx, dyy)，
Phi_d(dx, dy)为偏移的Cost权值。比如Phi_d(dx, dy) = (0, 0, 1, 1)则
di dp Phi_d(dx, dy)即为最普遍的欧氏距离。这一步称为距离变换，即下图中的transformed response。			   
*/
static void _ccv_dpm_compute_score(ccv_dpm_root_classifier_t* root_classifier, 
								   ccv_dense_matrix_t* hog, 
								   ccv_dense_matrix_t* hog2x, 
								   ccv_dense_matrix_t** _response, 
								   ccv_dense_matrix_t** part_feature, 
								   ccv_dense_matrix_t** dx, 
								   ccv_dense_matrix_t** dy)
{
	ccv_dense_matrix_t* response = 0;

	// 计算输入的根分类器在当前金字塔层的响应，输出到response
	ccv_filter(hog, root_classifier->root.w, &response, 0, CCV_NO_PADDING);
	ccv_dense_matrix_t* root_feature = 0;

	// 将response各通道相应的元素平均到root_feature相应的元素里面
	ccv_flatten(response, (ccv_matrix_t**)&root_feature, 0, 0);
	ccv_matrix_free(response);

	// 设置输出响应矩阵_response为root_feature
	*_response = root_feature;

	// 如果两倍空间分辨率的金字塔层为空则退出
	if (hog2x == 0)
		return;
	
	ccv_make_matrix_mutable(root_feature);

	// 计算根分类器w行列数的一半rwh,rww
	int rwh = (root_classifier->root.w->rows - 1) / 2, rww = (root_classifier->root.w->cols - 1) / 2;
	int rwh_1 = root_classifier->root.w->rows / 2, rww_1 = root_classifier->root.w->cols / 2;
	int i, x, y;

	for (i = 0; i < root_classifier->count; i++)
	{
		ccv_dpm_part_classifier_t* part = root_classifier->part + i;
		ccv_dense_matrix_t* response = 0;

		// 计算第i个部件分类器在当前两倍空间分辨率金字塔层的响应，输出到response
		// 部件滤波器位于根滤波器两倍空间分辨率的金字塔层
		ccv_filter(hog2x, part->w, &response, 0, CCV_NO_PADDING);
		ccv_dense_matrix_t* feature = 0;

		// 将每个部件的response各通道相应的元素平均到feature相应的元素里面
		ccv_flatten(response, (ccv_matrix_t**)&feature, 0, 0);

		// 释放响应矩阵
		ccv_matrix_free(response);

		// 部件特征，广义距离置零
		part_feature[i] = dx[i] = dy[i] = 0;

		// 将第i个部件特征feature做广义距离变换到part_feature[i]
		ccv_distance_transform(feature, 
							   &part_feature[i], 
							   0, 
							   &dx[i], 0, 
							   &dy[i], 0, 
							   part->dx, part->dy, part->dxx, part->dyy, 
							   CCV_NEGATIVE | CCV_GSEDT);

		ccv_matrix_free(feature);

		// 计算第i个部件分类器w行列数的一半pwh,pww
		int pwh = (part->w->rows - 1) / 2, pww = (part->w->cols - 1) / 2;

		// 计算第i个部件分类器到部件中心的y偏移offy
		int offy = part->y + pwh - rwh * 2;
		int miny = pwh, maxy = part_feature[i]->rows - part->w->rows + pwh;

		// 计算第i个部件分类器到部件中心的x偏移offx
		int offx = part->x + pww - rww * 2;
		int minx = pww, maxx = part_feature[i]->cols - part->w->cols + pww;

		// 从root_feature里获取rwh行0列的数据指针到f_ptr
		float* f_ptr = 
			(float*)ccv_get_dense_matrix_cell_by(CCV_32F | CCV_C1, root_feature, rwh, 0, 0);

		/* DPM的检测过程基于窗口扫描方法，对每个窗口计算如下分数： 
		窗口的分数 = 该位置根滤波器的分数 + ∑[i~m]max(第i个部件分数 - 第i个部件偏差)
		根滤波器和部件滤波器的分数是指滤波器与该滤波器中的HOG特征向量的内积，其
		中max的意思是，因为每个部件是可以移动的，每次移动都会产生一个分数和对应
		的偏差，在这个窗口内，我们要找出最佳的部件，也就是公式中提到的最大值。
		之后设定一个阈值，窗口分数高于阈值的则判别为目标。 
		*/
		for (y = rwh; y < root_feature->rows - rwh_1; y++)
		{
			// 计算第i个部件在部件金字塔y方向的偏移并在(miny, maxy)做饱和运算存到iy
			int iy = ccv_clamp(y * 2 + offy, miny, maxy);

			for (x = rww; x < root_feature->cols - rww_1; x++)
			{
				// 计算第i个部件在x方向的偏移并在(minx, maxx)做饱和运算存到ix
				int ix = ccv_clamp(x * 2 + offx, minx, maxx);

				// 从根特征值中减去第i个部件特征中的(iy, ix)元素
				f_ptr[x] -= 
					ccv_get_dense_matrix_cell_value_by(CCV_32F | CCV_C1, part_feature[i], iy, ix, 0);
			}

			// f_ptr指向根特征的下一行
			f_ptr += root_feature->cols;
		}
	}
}

#ifdef HAVE_LIBLINEAR
#ifdef HAVE_GSL

static uint64_t _ccv_dpm_time_measure()
{
	struct timeval tv;
	gettimeofday(&tv, 0);
	return tv.tv_sec * 1000000 + tv.tv_usec;
}

#define less_than(fn1, fn2, aux) ((fn1).value >= (fn2).value)
static CCV_IMPLEMENT_QSORT(_ccv_dpm_aspect_qsort, struct feature_node, less_than)
#undef less_than

#define less_than(a1, a2, aux) ((a1) < (a2))
static CCV_IMPLEMENT_QSORT(_ccv_dpm_area_qsort, int, less_than)
#undef less_than

#define less_than(s1, s2, aux) ((s1) < (s2))
static CCV_IMPLEMENT_QSORT(_ccv_dpm_score_qsort, double, less_than)
#undef less_than

static ccv_dpm_mixture_model_t* _ccv_dpm_model_copy(ccv_dpm_mixture_model_t* _model)
{
	ccv_dpm_mixture_model_t* model = (ccv_dpm_mixture_model_t*)ccmalloc(sizeof(ccv_dpm_mixture_model_t));
	model->count = _model->count;
	model->root = (ccv_dpm_root_classifier_t*)ccmalloc(sizeof(ccv_dpm_root_classifier_t) * model->count);
	int i, j;
	memcpy(model->root, _model->root, sizeof(ccv_dpm_root_classifier_t) * model->count);
	for (i = 0; i < model->count; i++)
	{
		ccv_dpm_root_classifier_t* _root = _model->root + i;
		ccv_dpm_root_classifier_t* root = model->root + i;
		root->root.w = ccv_dense_matrix_new(_root->root.w->rows, _root->root.w->cols, CCV_32F | 31, 0, 0);
		memcpy(root->root.w->data.u8, _root->root.w->data.u8, _root->root.w->rows * _root->root.w->step);
		ccv_make_matrix_immutable(root->root.w);
		ccv_dpm_part_classifier_t* _part = _root->part;
 		ccv_dpm_part_classifier_t* part = root->part = (ccv_dpm_part_classifier_t*)ccmalloc(sizeof(ccv_dpm_part_classifier_t) * root->count);
		memcpy(part, _part, sizeof(ccv_dpm_part_classifier_t) * root->count);
		for (j = 0; j < root->count; j++)
		{
			part[j].w = ccv_dense_matrix_new(_part[j].w->rows, _part[j].w->cols, CCV_32F | 31, 0, 0);
			memcpy(part[j].w->data.u8, _part[j].w->data.u8, _part[j].w->rows * _part[j].w->step);
			ccv_make_matrix_immutable(part[j].w);
		}
	}
	return model;
}

static void _ccv_dpm_write_checkpoint(ccv_dpm_mixture_model_t* model, int done, const char* dir)
{
	char swpfile[1024];
	sprintf(swpfile, "%s.swp", dir);
	FILE* w = fopen(swpfile, "w+");
	if (!w)
		return;
	if (done)
		fprintf(w, ".\n");
	else
		fprintf(w, ",\n");
	int i, j, x, y, ch, count = 0;
	for (i = 0; i < model->count; i++)
	{
		if (model->root[i].root.w == 0)
			break;
		count++;
	}
	if (done)
		fprintf(w, "%d\n", model->count);
	else
		fprintf(w, "%d %d\n", model->count, count);
	for (i = 0; i < count; i++)
	{
		ccv_dpm_root_classifier_t* root_classifier = model->root + i;
		fprintf(w, "%d %d\n", root_classifier->root.w->rows, root_classifier->root.w->cols);
		fprintf(w, "%a %a %a %a\n", root_classifier->beta, root_classifier->alpha[0], root_classifier->alpha[1], root_classifier->alpha[2]);
		ch = CCV_GET_CHANNEL(root_classifier->root.w->type);
		for (y = 0; y < root_classifier->root.w->rows; y++)
		{
			for (x = 0; x < root_classifier->root.w->cols * ch; x++)
				fprintf(w, "%a ", root_classifier->root.w->data.f32[y * root_classifier->root.w->cols * ch + x]);
			fprintf(w, "\n");
		}
		fprintf(w, "%d\n", root_classifier->count);
		for (j = 0; j < root_classifier->count; j++)
		{
			ccv_dpm_part_classifier_t* part_classifier = root_classifier->part + j;
			fprintf(w, "%d %d %d\n", part_classifier->x, part_classifier->y, part_classifier->z);
			fprintf(w, "%la %la %la %la\n", part_classifier->dx, part_classifier->dy, part_classifier->dxx, part_classifier->dyy);
			fprintf(w, "%a %a %a %a %a %a\n", part_classifier->alpha[0], part_classifier->alpha[1], part_classifier->alpha[2], part_classifier->alpha[3], part_classifier->alpha[4], part_classifier->alpha[5]);
			fprintf(w, "%d %d %d\n", part_classifier->w->rows, part_classifier->w->cols, part_classifier->counterpart);
			ch = CCV_GET_CHANNEL(part_classifier->w->type);
			for (y = 0; y < part_classifier->w->rows; y++)
			{
				for (x = 0; x < part_classifier->w->cols * ch; x++)
					fprintf(w, "%a ", part_classifier->w->data.f32[y * part_classifier->w->cols * ch + x]);
				fprintf(w, "\n");
			}
		}
	}
	fclose(w);
	rename(swpfile, dir);
}

static void _ccv_dpm_read_checkpoint(ccv_dpm_mixture_model_t* model, const char* dir)
{
	FILE* r = fopen(dir, "r");
	if (!r)
		return;
	int count;
	char flag;
	fscanf(r, "%c", &flag);
	assert(flag == ',');
	fscanf(r, "%d %d", &model->count, &count);
	ccv_dpm_root_classifier_t* root_classifier = (ccv_dpm_root_classifier_t*)ccmalloc(sizeof(ccv_dpm_root_classifier_t) * count);
	memset(root_classifier, 0, sizeof(ccv_dpm_root_classifier_t) * count);
	int i, j, k;
	for (i = 0; i < count; i++)
	{
		int rows, cols;
		fscanf(r, "%d %d", &rows, &cols);
		fscanf(r, "%f %f %f %f", &root_classifier[i].beta, &root_classifier[i].alpha[0], &root_classifier[i].alpha[1], &root_classifier[i].alpha[2]);
		root_classifier[i].root.w = ccv_dense_matrix_new(rows, cols, CCV_32F | 31, 0, 0);
		for (j = 0; j < rows * cols * 31; j++)
			fscanf(r, "%f", &root_classifier[i].root.w->data.f32[j]);
		ccv_make_matrix_immutable(root_classifier[i].root.w);
		fscanf(r, "%d", &root_classifier[i].count);
		if (root_classifier[i].count <= 0)
		{
			root_classifier[i].part = 0;
			continue;
		}
		ccv_dpm_part_classifier_t* part_classifier = (ccv_dpm_part_classifier_t*)ccmalloc(sizeof(ccv_dpm_part_classifier_t) * root_classifier[i].count);
		for (j = 0; j < root_classifier[i].count; j++)
		{
			fscanf(r, "%d %d %d", &part_classifier[j].x, &part_classifier[j].y, &part_classifier[j].z);
			fscanf(r, "%lf %lf %lf %lf", &part_classifier[j].dx, &part_classifier[j].dy, &part_classifier[j].dxx, &part_classifier[j].dyy);
			fscanf(r, "%f %f %f %f %f %f", &part_classifier[j].alpha[0], &part_classifier[j].alpha[1], &part_classifier[j].alpha[2], &part_classifier[j].alpha[3], &part_classifier[j].alpha[4], &part_classifier[j].alpha[5]);
			fscanf(r, "%d %d %d", &rows, &cols, &part_classifier[j].counterpart);
			part_classifier[j].w = ccv_dense_matrix_new(rows, cols, CCV_32F | 31, 0, 0);
			for (k = 0; k < rows * cols * 31; k++)
				fscanf(r, "%f", &part_classifier[j].w->data.f32[k]);
			ccv_make_matrix_immutable(part_classifier[j].w);
		}
		root_classifier[i].part = part_classifier;
	}
	model->root = root_classifier;
	fclose(r);
}

static void _ccv_dpm_mixture_model_cleanup(ccv_dpm_mixture_model_t* model)
{
	/* this is different because it doesn't compress to a continuous memory region */
	int i, j;
	
	for (i = 0; i < model->count; i++)
	{
		ccv_dpm_root_classifier_t* root_classifier = model->root + i;
		
		for (j = 0; j < root_classifier->count; j++)
		{
			ccv_dpm_part_classifier_t* part_classifier = root_classifier->part + j;
			ccv_matrix_free(part_classifier->w);
		}
		
		if (root_classifier->count > 0)
			ccfree(root_classifier->part);
		
		if (root_classifier->root.w != 0)
			ccv_matrix_free(root_classifier->root.w);
	}
	
	ccfree(model->root);
	model->count = 0;
	model->root = 0;
}

static const int _ccv_dpm_sym_lut[] = 
{ 
	2, 3, 0, 1,
	4 + 0, 4 + 8, 4 + 7, 4 + 6, 4 + 5, 4 + 4, 4 + 3, 4 + 2, 4 + 1,
	13 + 9, 13 + 8, 13 + 7, 13 + 6, 13 + 5, 13 + 4, 13 + 3, 13 + 2, 13 + 1, 13, 13 + 17, 13 + 16, 13 + 15, 13 + 14, 13 + 13, 13 + 12, 13 + 11, 13 + 10 
};

static void _ccv_dpm_check_root_classifier_symmetry(ccv_dense_matrix_t* w)
{
	assert(CCV_GET_CHANNEL(w->type) == 31 && CCV_GET_DATA_TYPE(w->type) == CCV_32F);
	float *w_ptr = w->data.f32;
	int i, j, k;
	
	for (i = 0; i < w->rows; i++)
	{
		for (j = 0; j < w->cols; j++)
		{
			for (k = 0; k < 31; k++)
			{
				double v = fabs(w_ptr[j * 31 + k] - w_ptr[(w->cols - 1 - j) * 31 + _ccv_dpm_sym_lut[k]]);

				if (v > 0.002)
					PRINT(CCV_CLI_INFO, "symmetric violation at (%d, %d, %d), off by: %f\n", i, j, k, v);
			}
		}
		
		w_ptr += w->cols * 31;
	}
}

typedef struct 
{
	int id;
	int count;
	float score;
	int x, y;
	float scale_x, scale_y;
	ccv_dpm_part_classifier_t root;
	ccv_dpm_part_classifier_t* part;
} ccv_dpm_feature_vector_t;

// rows cols根据前1/5正样本的最大长宽来计算DPM扫描块矩阵的行列
static void 
_ccv_dpm_collect_examples_randomly(gsl_rng* rng, 
								   ccv_array_t** negex, 
								   char** bgfiles, 
								   int bgnum, 
								   int negnum, 
								   int components, 
								   int* rows, 
								   int* cols, 
								   int grayscale)
{
	int i, j;

	// 按组件数循环
	for (i = 0; i < components; i++)
	{
		// 创建负样本矩阵 --negative-count 12000
		negex[i] = ccv_array_new(sizeof(ccv_dpm_feature_vector_t), negnum, 0);
	}

	// rows,cols上层根据前1/5正样本的最大长宽来计算DPM扫描块矩阵的行列
	int mrows = rows[0], mcols = cols[0];
	
	for (i = 1; i < components; i++)
	{
		// 获取DPM扫描块矩阵的行列的最大值
		mrows = ccv_max(mrows, rows[i]);
		mcols = ccv_max(mcols, cols[i]);
	}
	
	FLUSH(CCV_CLI_INFO, " - generating negative examples for all models : 0 / %d", negnum);

	// 当负样本矩阵元素个数小于设定的负样本总数
	while (negex[0]->rnum < negnum)
	{
		// 计算负样本总数和背景文件个数的比例p
		double p = (double)negnum / (double)bgnum;

		// 按背景文件个数循环
		for (i = 0; i < bgnum; i++)
		{
			// 产生随机数rng
			// Function 1.3：double gsl_rng_uniform (const gsl_rng *r)
			// 产生一个在[0, 1)区间上的双精度随机数，这也是科学计算最常用。它产生的随机数包括0，但不包括1。
			if (gsl_rng_uniform(rng) < p)
			{
				ccv_dense_matrix_t* image = 0;

				// 从第i个背景文件中读取图像放到image
				ccv_read(bgfiles[i], &image, (grayscale ? CCV_IO_GRAY : 0) | CCV_IO_ANY_FILE);
				assert(image != 0);
				
				if (image->rows - mrows * CCV_DPM_WINDOW_SIZE < 0 ||
					image->cols - mcols * CCV_DPM_WINDOW_SIZE < 0)
				{
					ccv_matrix_free(image);
					continue;
				}

				// 随机产生偏移坐标y,x
				// 产生均匀分布的随机数的函数
				int y = gsl_rng_uniform_int(rng, image->rows - mrows * CCV_DPM_WINDOW_SIZE + 1);
				int x = gsl_rng_uniform_int(rng, image->cols - mcols * CCV_DPM_WINDOW_SIZE + 1);

				// 按组件数循环
				for (j = 0; j < components; j++)
				{
					ccv_dense_matrix_t* slice = 0;

					// 从image中随机取一块放到slice
					// 将该背景图像划分到slice
					ccv_slice(image, 
							  (ccv_matrix_t**)&slice, 
							  0, 
							  y + ((mrows - rows[j]) * CCV_DPM_WINDOW_SIZE + 1) / 2, 
							  x + ((mcols - cols[j]) * CCV_DPM_WINDOW_SIZE + 1) / 2, 
							  rows[j] * CCV_DPM_WINDOW_SIZE, 
							  cols[j] * CCV_DPM_WINDOW_SIZE);

					assert(y + ((mrows - rows[j]) * CCV_DPM_WINDOW_SIZE + 1) / 2 >= 0 &&
						   y + ((mrows - rows[j]) * CCV_DPM_WINDOW_SIZE + 1) / 2 + rows[j] * CCV_DPM_WINDOW_SIZE <= image->rows &&
						   x + ((mcols - cols[j]) * CCV_DPM_WINDOW_SIZE + 1) / 2 >= 0 &&
						   x + ((mcols - cols[j]) * CCV_DPM_WINDOW_SIZE + 1) / 2 + cols[j] * CCV_DPM_WINDOW_SIZE <= image->cols);
					ccv_dense_matrix_t* hog = 0;

					// 将该随机块背景图做HOG运算到hog
					ccv_hog(slice, &hog, 0, 9, CCV_DPM_WINDOW_SIZE);
					ccv_matrix_free(slice);
					
					ccv_dpm_feature_vector_t vector = 
					{
						.id = j,
						.count = 0,
						.part = 0,
					};
					ccv_make_matrix_mutable(hog);
					assert(hog->rows == rows[j] && hog->cols == cols[j] && CCV_GET_CHANNEL(hog->type) == 31 && CCV_GET_DATA_TYPE(hog->type) == CCV_32F);
					vector.root.w = hog;

					// 将该背景HOG矩阵压栈到negex
					ccv_array_push(negex[j], &vector);
				}
				
				ccv_matrix_free(image);
				FLUSH(CCV_CLI_INFO, " - generating negative examples for all models : %d / %d", negex[0]->rnum, negnum);

				// 如果堆栈元素个数大于负样本总数则退出
				if (negex[0]->rnum >= negnum)
					break;
			}
		}
	}
}


static ccv_array_t* 
_ccv_dpm_summon_examples_by_rectangle(char** posfiles, 
									  ccv_rect_t* bboxes, 
									  int posnum, 
									  int id, 
									  int rows, 
									  int cols, 
									  int grayscale)
{
	int i;
	FLUSH(CCV_CLI_INFO, " - generating positive examples for model %d : 0 / %d", id, posnum);

	// 按正样本数目创建正样本向量矩阵posv
	ccv_array_t* posv = ccv_array_new(sizeof(ccv_dpm_feature_vector_t), posnum, 0);

	// 按正样本数目循环
	for (i = 0; i < posnum; i++)
	{
		// 获取该正样本的包围框
		ccv_rect_t bbox = bboxes[i];

		// 计算该正样本经过平均长宽比缩放过后的列和行
		int mcols = (int)(sqrtf(bbox.width * bbox.height * cols / (float)rows) + 0.5);
		int mrows = (int)(sqrtf(bbox.width * bbox.height * rows / (float)cols) + 0.5);

		// 设置该正样本经过平均长宽比变换过后的坐标和长宽
		bbox.x = bbox.x + (bbox.width - mcols) / 2;
		bbox.y = bbox.y + (bbox.height - mrows) / 2;
		bbox.width = mcols;
		bbox.height = mrows;

		ccv_dpm_feature_vector_t vector = 
		{
			.id = id,
			.count = 0,
			.part = 0,
		};

		// 分辨率太小而不能用
		// resolution is too low to be useful
		if (mcols * 2 < cols * CCV_DPM_WINDOW_SIZE || mrows * 2 < rows * CCV_DPM_WINDOW_SIZE)
		{
			vector.root.w = 0;

			// 如果分辨率太小则不做HOG直接压栈
			ccv_array_push(posv, &vector);

			continue;
		}
		
		ccv_dense_matrix_t* image = 0;

		// 将该正样本文件数据读到image里
		ccv_read(posfiles[i], &image, (grayscale ? CCV_IO_GRAY : 0) | CCV_IO_ANY_FILE);
		assert(image != 0);
		ccv_dense_matrix_t* up2x = 0;

		// 将该正样本图像上采样到up2x
		ccv_sample_up(image, &up2x, 0, 0, 0);
		ccv_matrix_free(image);
		ccv_dense_matrix_t* slice = 0;

		// 将该正样本上采样图像按包围盒的位置取一块放到slice
		ccv_slice(up2x, (ccv_matrix_t**)&slice, 0, bbox.y * 2, bbox.x * 2, bbox.height * 2, bbox.width * 2);
		ccv_matrix_free(up2x);
		ccv_dense_matrix_t* resize = 0;

		// 将该正样本划分resize到resize
		ccv_resample(slice, &resize, 0, rows * CCV_DPM_WINDOW_SIZE, cols * CCV_DPM_WINDOW_SIZE, CCV_INTER_AREA);
		ccv_matrix_free(slice);
		ccv_dense_matrix_t* hog = 0;

		// 将该正样本尺度归一化后的图像做HOG运算
		ccv_hog(resize, &hog, 0, 9, CCV_DPM_WINDOW_SIZE);
		ccv_matrix_free(resize);
		ccv_make_matrix_mutable(hog);
		assert(hog->rows == rows && hog->cols == cols && CCV_GET_CHANNEL(hog->type) == 31 && CCV_GET_DATA_TYPE(hog->type) == CCV_32F);
		vector.root.w = hog;

		// 将HOG矩阵压栈到posv
		ccv_array_push(posv, &vector);
		FLUSH(CCV_CLI_INFO, " - generating positive examples for model %d : %d / %d", id, i + 1, posnum);
	}

	return posv;
}

static void _ccv_dpm_initialize_root_classifier(gsl_rng* rng, 
												ccv_dpm_root_classifier_t* root_classifier, 
												int label, 
												int cnum, 
												int* poslabels, 
												ccv_array_t* posex, 
												int* neglabels, 
												ccv_array_t* negex, 
												double C, 
												int symmetric, 
												int grayscale)
{
	int i, j, x, y, k, l;
	int cols = root_classifier->root.w->cols;
	int cols2c = (cols + 1) / 2;
	int rows = root_classifier->root.w->rows;
	PRINT(CCV_CLI_INFO, " - creating initial model %d at %dx%d\n", label + 1, cols, rows);
	struct problem prob;
	prob.n = symmetric ? 31 * cols2c * rows + 1 : 31 * cols * rows + 1;
	prob.bias = symmetric ? 0.5 : 1.0; // for symmetric, since we only pass half features in, need to set bias to be half too
	// new version (1.91) of liblinear uses double instead of int (1.8) for prob.y, cannot cast for that.
	prob.y = malloc(sizeof(prob.y[0]) * (cnum + negex->rnum) * (!!symmetric + 1));
	prob.x = (struct feature_node**)malloc(sizeof(struct feature_node*) * (cnum + negex->rnum) * (!!symmetric + 1));
	FLUSH(CCV_CLI_INFO, " - converting examples to liblinear format: %d / %d", 0, (cnum + negex->rnum) * (!!symmetric + 1));
	l = 0;

	// 初始化正样本特征向量
	for (i = 0; i < posex->rnum; i++)
	{
		// 如果当前正样本标签==当前组件号
		if (poslabels[i] == label)
		{
			// 获取当前正样本向量的根w到hog
			ccv_dense_matrix_t* hog = ((ccv_dpm_feature_vector_t*)ccv_array_get(posex, i))->root.w;

			if (!hog)
				continue;

			// 定义特征节点指针features
			/* 作者对HOG进行了很大的改动。作者没有用4*9=36维向量，而是对每个8x8
			的cell提取18+9+4=31维特征向量。作者还讨论了依据PCA（Principle Component 
			Analysis）可视化的结果选9+4维特征，能达到HOG 4*9维特征的效果。*/
			struct feature_node* features;

			// 如果对称
			if (symmetric)
			{
				features = 
					(struct feature_node*)malloc(sizeof(struct feature_node) * (31 * cols2c * rows + 2));

				// 定义指向当前正样本向量hog数据的指针hptr
				float* hptr = hog->data.f32;
				j = 0;
				
				for (y = 0; y < rows; y++)
				{
					for (x = 0; x < cols2c; x++)
					{
						for (k = 0; k < 31; k++)
						{
							// 将当前正样本向量hog的元素拷贝到features
							features[j].index = j + 1;
							features[j].value = hptr[x * 31 + k];
							++j;
						}
					}
					
					hptr += hog->cols * 31;
				}
				
				features[j].index = j + 1;
				features[j].value = prob.bias;
				features[j + 1].index = -1;

				// 将当前正样本的所有特征赋给prob.x[l]
				prob.x[l] = features;
				prob.y[l] = 1;
				++l;

				// 重新给特征节点指针features分配空间
				features = 
					(struct feature_node*)malloc(sizeof(struct feature_node) * (31 * cols2c * rows + 2));
				hptr = hog->data.f32;

				j = 0;
				
				for (y = 0; y < rows; y++)
				{
					for (x = 0; x < cols2c; x++)
					{
						for (k = 0; k < 31; k++)
						{
							// 将对称的hog元素拷贝到features
							features[j].index = j + 1;
							features[j].value = hptr[(cols - 1 - x) * 31 + _ccv_dpm_sym_lut[k]];
							++j;
						}
					}
					
					hptr += hog->cols * 31;
				}
				
				features[j].index = j + 1;
				features[j].value = prob.bias;
				features[j + 1].index = -1;

				// 将当前正样本的所有对称特征赋给prob.x[l]
				prob.x[l] = features;
				prob.y[l] = 1;
				++l;
			} 
			else // 如果不对称
			{
				features = (struct feature_node*)malloc(sizeof(struct feature_node) * (31 * cols * rows + 2));

				for (j = 0; j < rows * cols * 31; j++)
				{	
					// 将当前正样本向量hog的元素拷贝到features
					features[j].index = j + 1;
					features[j].value = hog->data.f32[j];
				}
				
				features[31 * rows * cols].index = 31 * rows * cols + 1;
				features[31 * rows * cols].value = prob.bias;
				features[31 * rows * cols + 1].index = -1;

				// 将当前正样本的所有对称特征赋给prob.x[l]
				prob.x[l] = features;
				prob.y[l] = 1;
				++l;
			}
			
			FLUSH(CCV_CLI_INFO, " - converting examples to liblinear format: %d / %d", l, (cnum + negex->rnum) * (!!symmetric + 1));
		}
	}

	// 初始化负样本特征向量
	for (i = 0; i < negex->rnum; i++)
	{
		if (neglabels[i] == label)
		{
			ccv_dense_matrix_t* hog = ((ccv_dpm_feature_vector_t*)ccv_array_get(negex, i))->root.w;
			struct feature_node* features;

			if (symmetric)
			{
				features = (struct feature_node*)malloc(sizeof(struct feature_node) * (31 * cols2c * rows + 2));
				float* hptr = hog->data.f32;
				j = 0;
				
				for (y = 0; y < rows; y++)
				{
					for (x = 0; x < cols2c; x++)
						for (k = 0; k < 31; k++)
						{
							features[j].index = j + 1;
							features[j].value = hptr[x * 31 + k];
							++j;
						}
						
					hptr += hog->cols * 31;
				}
				
				features[j].index = j + 1;
				features[j].value = prob.bias;
				features[j + 1].index = -1;
				prob.x[l] = features;
				prob.y[l] = -1;
				++l;
				features = (struct feature_node*)malloc(sizeof(struct feature_node) * (31 * cols2c * rows + 2));
				hptr = hog->data.f32;
				j = 0;
				
				for (y = 0; y < rows; y++)
				{
					for (x = 0; x < cols2c; x++)
						for (k = 0; k < 31; k++)
						{
							features[j].index = j + 1;
							features[j].value = hptr[(cols - 1 - x) * 31 + _ccv_dpm_sym_lut[k]];
							++j;
						}
					hptr += hog->cols * 31;
				}
				
				features[j].index = j + 1;
				features[j].value = prob.bias;
				features[j + 1].index = -1;
				prob.x[l] = features;
				prob.y[l] = -1;
				++l;
			}
			else 
			{
				features = (struct feature_node*)malloc(sizeof(struct feature_node) * (31 * cols * rows + 2));

				for (j = 0; j < 31 * rows * cols; j++)
				{
					features[j].index = j + 1;
					features[j].value = hog->data.f32[j];
				}
				
				features[31 * rows * cols].index = 31 * rows * cols + 1;
				features[31 * rows * cols].value = prob.bias;
				features[31 * rows * cols + 1].index = -1;
				prob.x[l] = features;
				prob.y[l] = -1;
				++l;
			}
			
			FLUSH(CCV_CLI_INFO, " - converting examples to liblinear format: %d / %d", l, (cnum + negex->rnum) * (!!symmetric + 1));
		}
	}
		
	prob.l = l;
	PRINT(CCV_CLI_INFO, "\n - generated %d examples with %d dimensions each\n"
						" - running liblinear for initial linear SVM model (L2-regularized, L1-loss)\n", prob.l, prob.n);

	struct parameter linear_parameters = { .solver_type = L2R_L1LOSS_SVC_DUAL,
										   .eps = 1e-1,
										   .C = C,
										   .nr_weight = 0,
										   .weight_label = 0,
										   .weight = 0 };
	const char* err = check_parameter(&prob, &linear_parameters);
	
	if (err)
	{
		PRINT(CCV_CLI_ERROR, " - ERROR: cannot pass check parameter: %s\n", err);
		exit(-1);
	}

	// 根据已有的特征向量和参数训练模型linear
	struct model* linear = train(&prob, &linear_parameters);
	assert(linear != 0);
	PRINT(CCV_CLI_INFO, " - model->label[0]: %d, model->nr_class: %d, model->nr_feature: %d\n", linear->label[0], linear->nr_class, linear->nr_feature);

	// 初始化模型Beta
	// Data 3: Initial model Beta
	// 如果对称
	if (symmetric)
	{
		float* wptr = root_classifier->root.w->data.f32;
		
		for (y = 0; y < rows; y++)
		{
			for (x = 0; x < cols2c; x++)
			{
				for (k = 0; k < 31; k++)
				{
					// 从训练好的初始模型中获取w到wptr
					wptr[(cols - 1 - x) * 31 + _ccv_dpm_sym_lut[k]] = 
						wptr[x * 31 + k] = linear->w[(y * cols2c + x) * 31 + k];
				}
			}
			
			wptr += cols * 31;
		}

		// 因为对称性，lsvm只计算一半特征，为了补偿它，将常数翻倍
		// since for symmetric, lsvm only computed half features, to compensate that, we doubled the constant.
		root_classifier->beta = linear->w[31 * rows * cols2c] * 2.0;
	} 
	else // 如果不对称
	{
		for (j = 0; j < 31 * rows * cols; j++)
		{
			// 从训练好的初始模型中获取w到根分类器的根w
			root_classifier->root.w->data.f32[j] = linear->w[j];
		}

		// 从训练好的初始模型中的最后一项获取beta到根分类器的beta
		root_classifier->beta = linear->w[31 * rows * cols];
	}
	
	free_and_destroy_model(&linear);
	free(prob.y);

	for (j = 0; j < prob.l; j++)
		free(prob.x[j]);

	free(prob.x);
	ccv_make_matrix_immutable(root_classifier->root.w);
}

static void 
_ccv_dpm_initialize_part_classifiers(ccv_dpm_root_classifier_t* root_classifier, 
									 int parts, 
									 int symmetric)
{
	int i, j, k, x, y;
	ccv_dense_matrix_t* w = 0;

	// 上采样输入的根分类器的根w到w
	ccv_sample_up(root_classifier->root.w, &w, 0, 0, 0);
	ccv_make_matrix_mutable(w);

	// 根据命令行输入的参数设置该根分类器的部件数
	root_classifier->count = parts;

	// 初始化部件空间
	root_classifier->part = (ccv_dpm_part_classifier_t*)ccmalloc(sizeof(ccv_dpm_part_classifier_t) * parts);
	memset(root_classifier->part, 0, sizeof(ccv_dpm_part_classifier_t) * parts);

	// 计算分摊到每个部件的w的面积area
	double area = w->rows * w->cols / (double)parts;

	// 按部件数循环
	for (i = 0; i < parts;)
	{
		// 获取第i个部件分类器指针
		ccv_dpm_part_classifier_t* part_classifier = root_classifier->part + i;
		int dx = 0, dy = 0, dw = 0, dh = 0, sym = 0;
		double dsum = -1.0; // absolute value, thus, -1.0 is enough

		// 如果需要则划分和更新
#define slice_and_update_if_needed(y, x, l, n, s) \
		{ \
			ccv_dense_matrix_t* slice = 0; \
			ccv_slice(w, (ccv_matrix_t**)&slice, 0, y, x, l, n); \
			double sum = ccv_sum(slice, CCV_UNSIGNED) / (double)(l * n); \
			if (sum > dsum) \
			{ \
				dsum = sum; \
				dx = x; \
				dy = y; \
				dw = n; \
				dh = l; \
				sym = s; \
			} \
			ccv_matrix_free(slice); \
		}

		for (j = 1; (j < area + 1) && (j * 3 <= w->rows * 2); j++)
		{
			k = (int)(area / j + 0.5);
			
			if (k < 1 || k * 3 > w->cols * 2)
				continue;

			if (j > k * 2 || k > j * 2)
				continue;

			// 如果对称
			if (symmetric)
			{
				// 可以关于水平中心对称
				if (k % 2 == w->cols % 2) // can be symmetric in horizontal center
				{
					x = (w->cols - k) / 2;
					for (y = 0; y < w->rows - j + 1; y++)
						slice_and_update_if_needed(y, x, j, k, 0);
				}

				// 有两个位置
				if (i < parts - 1) // have 2 locations
				{
					for (y = 0; y < w->rows - j + 1; y++)
						for (x = 0; x <= w->cols / 2 - k /* to avoid overlapping */; x++)
							slice_and_update_if_needed(y, x, j, k, 1);
				}
			} 
			else 
			{
				for (y = 0; y < w->rows - j + 1; y++)
				{
					for (x = 0; x < w->cols - k + 1; x++)
					{
						// 如果w的y, x, j, k划分的矩阵平均值>dsum,则更新dsum,dx,dy,dw,dh,sym
						slice_and_update_if_needed(y, x, j, k, 0);
					}
				}
			}
		}
		
		PRINT(CCV_CLI_INFO, " ---- part %d(%d) %dx%d at (%d,%d), entropy: %lf\n", i + 1, parts, dw, dh, dx, dy, dsum);

		// 初始化部件分类器0的参数
		part_classifier->dx = 0;
		part_classifier->dy = 0;
		part_classifier->dxx = 0.1f;
		part_classifier->dyy = 0.1f;
		part_classifier->x = dx;
		part_classifier->y = dy;
		part_classifier->z = 1;
		part_classifier->w = 0;

		// 按上面更新的dy, dx, dh, dw来把w划分到part_classifier->w
		ccv_slice(w, (ccv_matrix_t**)&part_classifier->w, 0, dy, dx, dh, dw);
		ccv_make_matrix_immutable(part_classifier->w);

		// 获取该划分的指针w_ptr
		// 清空已选择的区域
		/* clean up the region we selected */
		float* w_ptr = (float*)ccv_get_dense_matrix_cell_by(CCV_32F | 31, w, dy, dx, 0);

		// w_ptr的数据置零
		for (y = 0; y < dh; y++)
		{
			for (x = 0; x < dw * 31; x++)
				w_ptr[x] = 0;
			
			w_ptr += w->cols * 31;
		}

		// 部件数+1
		i++;

		// 添加配对部件
		if (symmetric && sym) // add counter-part
		{
			// dx在x方向做镜像
			dx = w->cols - (dx + dw);
			PRINT(CCV_CLI_INFO, " ---- part %d(%d) %dx%d at (%d,%d), entropy: %lf\n", i + 1, parts, dw, dh, dx, dy, dsum);

			// 初始化部件分类器1的参数
			part_classifier[1].dx = 0;
			part_classifier[1].dy = 0;
			part_classifier[1].dxx = 0.1f;
			part_classifier[1].dyy = 0.1f;
			part_classifier[1].x = dx;
			part_classifier[1].y = dy;
			part_classifier[1].z = 1;
			part_classifier[1].w = 0;

			// 按上面更新的dy, dx, dh, dw来把w划分到part_classifier[1].w
			ccv_slice(w, (ccv_matrix_t**)&part_classifier[1].w, 0, dy, dx, dh, dw);
			ccv_make_matrix_immutable(part_classifier[1].w);

			// 清空已选择的区域
			/* clean up the region we selected */
			float* w_ptr = (float*)ccv_get_dense_matrix_cell_by(CCV_32F | 31, w, dy, dx, 0);

			for (y = 0; y < dh; y++)
			{
				for (x = 0; x < dw * 31; x++)
					w_ptr[x] = 0;
				w_ptr += w->cols * 31;
			}
			
			part_classifier[0].counterpart = i;
			part_classifier[1].counterpart = i - 1;

			// 部件数+1
			i++;
		} 
		else 
		{
			part_classifier->counterpart = -1;
		}
	}
	
	ccv_matrix_free(w);
}

static void 
_ccv_dpm_initialize_feature_vector_on_pattern(ccv_dpm_feature_vector_t* vector, 
											  ccv_dpm_root_classifier_t* root, 
											  int id)
{
	int i;
	vector->id = id;

	// 设置部件个数
	vector->count = root->count;
	vector->part = 
		(ccv_dpm_part_classifier_t*)ccmalloc(sizeof(ccv_dpm_part_classifier_t) * root->count);
	vector->root.w = 
		ccv_dense_matrix_new(root->root.w->rows, root->root.w->cols, CCV_32F | 31, 0, 0);

	for (i = 0; i < vector->count; i++)
	{
		vector->part[i].x = root->part[i].x;
		vector->part[i].y = root->part[i].y;
		vector->part[i].z = root->part[i].z;
		vector->part[i].w = 
			ccv_dense_matrix_new(root->part[i].w->rows, 
								 root->part[i].w->cols, 
								 CCV_32F | 31, 
								 0, 
								 0);
	}
}

static void _ccv_dpm_feature_vector_cleanup(ccv_dpm_feature_vector_t* vector)
{
	int i;
	if (vector->root.w)
		ccv_matrix_free(vector->root.w);
	for (i = 0; i < vector->count; i++)
		ccv_matrix_free(vector->part[i].w);
	if (vector->part)
		ccfree(vector->part);
}

static void _ccv_dpm_feature_vector_free(ccv_dpm_feature_vector_t* vector)
{
	_ccv_dpm_feature_vector_cleanup(vector);
	
	ccfree(vector);
}

static double _ccv_dpm_vector_score(ccv_dpm_mixture_model_t* model, 
									ccv_dpm_feature_vector_t* v)
{
	if (v->id < 0 || v->id >= model->count)
		return 0;
	
	ccv_dpm_root_classifier_t* root_classifier = model->root + v->id;

	// b是为了component之间对齐而设的rootoffset
	double score = root_classifier->beta;
	int i, k, ch = CCV_GET_CHANNEL(v->root.w->type);
	assert(ch == 31);
	float *vptr = v->root.w->data.f32;
	float *wptr = root_classifier->root.w->data.f32;
	
	for (i = 0; i < v->root.w->rows * v->root.w->cols * ch; i++)
		score += wptr[i] * vptr[i];

	assert(v->count == root_classifier->count || (v->count == 0 && v->part == 0));

	// 
	for (k = 0; k < v->count; k++)
	{
		ccv_dpm_part_classifier_t* part_classifier = root_classifier->part + k;
		ccv_dpm_part_classifier_t* part_vector = v->part + k;

		// (dx,dy)为偏移向量,di为偏移向量(dx,dy,dxx,dyy),PhiD(dx,dy)为偏移的Cost权值 
		// 比如PhiD(dx,dy) = (0,0,1,1),则di dp PhiD(dx,dy)即为最普遍的欧氏距离。
		// 这一步称为距离变换，即transformed response
		score -= part_classifier->dx * part_vector->dx;
		score -= part_classifier->dxx * part_vector->dxx;
		score -= part_classifier->dy * part_vector->dy;
		score -= part_classifier->dyy * part_vector->dyy;
		vptr = part_vector->w->data.f32;
		wptr = part_classifier->w->data.f32;

		// 中间是n个partfilter（前面称之为子模型）的得分。
		for (i = 0; i < part_vector->w->rows * part_vector->w->cols * ch; i++)
		{
			score += wptr[i] * vptr[i];
		}
	}
	
	return score;
}

static void 
_ccv_dpm_collect_feature_vector(ccv_dpm_feature_vector_t* v, 
								float score, 
								int x, 
								int y, 
								ccv_dense_matrix_t* pyr, // pyr[j], 
								ccv_dense_matrix_t* detail, // pyr[j - next], 
								ccv_dense_matrix_t** dx, 
								ccv_dense_matrix_t** dy)
{
	v->score = score;
	v->x = x;
	v->y = y;

	// 将特征向量v的根w置零
	ccv_zero(v->root.w);

	// 计算特征向量v的根w行列的一半
	int rwh = (v->root.w->rows - 1) / 2, rww = (v->root.w->cols - 1) / 2;
	int i, ix, iy, ch = CCV_GET_CHANNEL(v->root.w->type);

	// 获取金字塔的y - rwh, x - rww元素指针
	float* h_ptr = (float*)ccv_get_dense_matrix_cell_by(CCV_32F | ch, pyr, y - rwh, x - rww, 0);

	// 定义指向特征向量v的根w的数据指针w_ptr
	float* w_ptr = v->root.w->data.f32;

	for (iy = 0; iy < v->root.w->rows; iy++)
	{
		// 拷贝金字塔的一行到特征向量v的根w
		memcpy(w_ptr, h_ptr, v->root.w->cols * ch * sizeof(float));

		// h_ptr指向金字塔的下一行
		h_ptr += pyr->cols * ch;

		// w_ptr指向特征向量v的根w的下一行
		w_ptr += v->root.w->cols * ch;
	}
	
	for (i = 0; i < v->count; i++)
	{
		ccv_dpm_part_classifier_t* part = v->part + i;

		// 计算部件宽高的一半
		int pww = (part->w->cols - 1) / 2, pwh = (part->w->rows - 1) / 2;

		// 计算部件相对于根的偏移
		int offy = part->y + pwh - rwh * 2;
		int offx = part->x + pww - rww * 2;
		iy = ccv_clamp(y * 2 + offy, pwh, detail->rows - part->w->rows + pwh);
		ix = ccv_clamp(x * 2 + offx, pww, detail->cols - part->w->cols + pww);
		int ry = ccv_get_dense_matrix_cell_value_by(CCV_32S | CCV_C1, dy[i], iy, ix, 0);
		int rx = ccv_get_dense_matrix_cell_value_by(CCV_32S | CCV_C1, dx[i], iy, ix, 0);

		// 计算形变特征向量
		part->dx = rx; // I am not sure if I need to flip the sign or not (confirmed, it should be this way)
		part->dy = ry;
		part->dxx = rx * rx;
		part->dyy = ry * ry;

		// 处理越界错误
		// deal with out-of-bound error
		int start_y = ccv_max(0, iy - ry - pwh);
		assert(start_y < detail->rows);
		int start_x = ccv_max(0, ix - rx - pww);
		assert(start_x < detail->cols);
		int end_y = ccv_min(detail->rows, iy - ry - pwh + part->w->rows);
		assert(end_y >= 0);
		int end_x = ccv_min(detail->cols, ix - rx - pww + part->w->cols);
		assert(end_x >= 0);

		// 获取detail指针的start_y行start_x列0通道处的值
		h_ptr = (float*)ccv_get_dense_matrix_cell_by(CCV_32F | ch, detail, start_y, start_x, 0);

		// 将第i个部件分类器的w置零
		ccv_zero(v->part[i].w);

		// 获取part->w指针的start_y - (iy - ry - pwh)行start_x - (ix - rx - pww)列0通道处的值
		w_ptr = (float*)ccv_get_dense_matrix_cell_by(CCV_32F | ch, 
													 part->w, 
													 start_y - (iy - ry - pwh), 
													 start_x - (ix - rx - pww), 
													 0);

		for (iy = start_y; iy < end_y; iy++)
		{
			// 拷贝2x金字塔的一行到特征向量v的部件w
			memcpy(w_ptr, h_ptr, (end_x - start_x) * ch * sizeof(float));
			h_ptr += detail->cols * ch;
			w_ptr += part->w->cols * ch;
		}
	}
}

static ccv_dpm_feature_vector_t* 
_ccv_dpm_collect_best(ccv_dense_matrix_t* image, 
					  ccv_dpm_mixture_model_t* model, 
					  ccv_rect_t bbox, 
					  double overlap, 
					  ccv_dpm_param_t params)
{
	int i, j, k, x, y;
	double scale = pow(2.0, 1.0 / (params.interval + 1.0));
	int next = params.interval + 1;
	int scale_upto = _ccv_dpm_scale_upto(image, &model, 1, params.interval);
	if (scale_upto < 0)
		return 0;

	// 分配特征金字塔空间
	ccv_dense_matrix_t** pyr = (ccv_dense_matrix_t**)alloca((scale_upto + next * 2) * sizeof(ccv_dense_matrix_t*));

	// 生成特征金字塔pyr
	_ccv_dpm_feature_pyramid(image, pyr, scale_upto, params.interval);

	float best = -FLT_MAX;
	ccv_dpm_feature_vector_t* v = 0;

	// 按组件搜索
	for (i = 0; i < model->count; i++)
	{
		ccv_dpm_root_classifier_t* root_classifier = model->root + i;
		double scale_x = 1.0;
		double scale_y = 1.0;

		// 按尺度空间搜索
		for (j = next; j < scale_upto + next * 2; j++)
		{
			ccv_size_t size = 
				ccv_size((int)(root_classifier->root.w->cols * CCV_DPM_WINDOW_SIZE * scale_x + 0.5), 
						 (int)(root_classifier->root.w->rows * CCV_DPM_WINDOW_SIZE * scale_y + 0.5));

			if (ccv_min((double)(size.width * size.height), (double)(bbox.width * bbox.height)) / 
				ccv_max((double)(bbox.width * bbox.height), (double)(size.width * size.height)) < overlap)
			{
				scale_x *= scale;
				scale_y *= scale;
				continue;
			}
			
			ccv_dense_matrix_t* root_feature = 0;
			ccv_dense_matrix_t* part_feature[CCV_DPM_PART_MAX];
			ccv_dense_matrix_t* dx[CCV_DPM_PART_MAX];
			ccv_dense_matrix_t* dy[CCV_DPM_PART_MAX];

			// 计算根w部件w在两层金字塔的响应分数输出到root_feature,part_feature,dx,dy里面
			_ccv_dpm_compute_score(root_classifier, 
								   pyr[j], 
								   pyr[j - next], 
								   &root_feature, 
								   part_feature, 
								   dx, 
								   dy);

			// 定义根分类器根w行列的一半
			int rwh = (root_classifier->root.w->rows - 1) / 2, 
				rww = (root_classifier->root.w->cols - 1) / 2;
			int rwh_1 = root_classifier->root.w->rows / 2, 
				rww_1 = root_classifier->root.w->cols / 2;

			// 获取根特征矩阵的rwh行0列的元素指针
			float* f_ptr = 
				(float*)ccv_get_dense_matrix_cell_by(CCV_32F | CCV_C1, 
													 root_feature, 
													 rwh, 
													 0, 
													 0);

			// 找到最好的响应值best和对应的特征向量v
			for (y = rwh; y < root_feature->rows - rwh_1; y++)
			{
				for (x = rww; x < root_feature->cols - rww_1; x++)
				{
					ccv_rect_t rect = 
						ccv_rect((int)((x - rww) * CCV_DPM_WINDOW_SIZE * scale_x + 0.5), 
								 (int)((y - rwh) * CCV_DPM_WINDOW_SIZE * scale_y + 0.5), 
								 (int)(root_classifier->root.w->cols * CCV_DPM_WINDOW_SIZE * scale_x + 0.5), 
								 (int)(root_classifier->root.w->rows * CCV_DPM_WINDOW_SIZE * scale_y + 0.5));

					if ((double)(ccv_max(0, ccv_min(rect.x + rect.width, bbox.x + bbox.width) - ccv_max(rect.x, bbox.x)) 
					   *    	 ccv_max(0, ccv_min(rect.y + rect.height, bbox.y + bbox.height) - ccv_max(rect.y, bbox.y))) 
					   / (double)ccv_max(rect.width * rect.height, bbox.width * bbox.height) >= overlap 
					 && f_ptr[x] > best)
					{
						// 初始化v
						// initialize v
						if (v == 0)
						{
							v = (ccv_dpm_feature_vector_t*)ccmalloc(sizeof(ccv_dpm_feature_vector_t));

							// 按模式初始化特征向量v
							_ccv_dpm_initialize_feature_vector_on_pattern(v, root_classifier, i);
						}

						// 如果v->id是另外一种类型，则清空并重新初始化
						// if it is another kind, cleanup and reinitialize
						if (v->id != i)
						{
							_ccv_dpm_feature_vector_cleanup(v);
							_ccv_dpm_initialize_feature_vector_on_pattern(v, root_classifier, i);
						}

						// 按输入的根坐标x,y和部件坐标dx,dy从两层金字塔拷贝数据到v里面
						// 收集特征向量
						_ccv_dpm_collect_feature_vector(v, 
														f_ptr[x] + root_classifier->beta, // score
														x, 
														y, 
														pyr[j], 
														pyr[j - next], 
														dx, 
														dy);
						v->scale_x = scale_x;
						v->scale_y = scale_y;
						best = f_ptr[x];
					}
				}

				f_ptr += root_feature->cols;
			}
			
			for (k = 0; k < root_classifier->count; k++)
			{
				ccv_matrix_free(part_feature[k]);
				ccv_matrix_free(dx[k]);
				ccv_matrix_free(dy[k]);
			}
			
			ccv_matrix_free(root_feature);
			scale_x *= scale;
			scale_y *= scale;
		}
	}
	
	for (i = 0; i < scale_upto + next * 2; i++)
		ccv_matrix_free(pyr[i]);
	
	return v;
}

static ccv_array_t* 
_ccv_dpm_collect_all(gsl_rng* rng, 
					 ccv_dense_matrix_t* image, 
					 ccv_dpm_mixture_model_t* model, 
					 ccv_dpm_param_t params, 
					 float threshold)
{
	int i, j, k, x, y;
	double scale = pow(2.0, 1.0 / (params.interval + 1.0));
	int next = params.interval + 1;
	int scale_upto = _ccv_dpm_scale_upto(image, &model, 1, params.interval);

	if (scale_upto < 0)
		return 0;

	ccv_dense_matrix_t** pyr = (ccv_dense_matrix_t**)alloca((scale_upto + next * 2) * sizeof(ccv_dense_matrix_t*));
	_ccv_dpm_feature_pyramid(image, pyr, scale_upto, params.interval);
	ccv_array_t* av = ccv_array_new(sizeof(ccv_dpm_feature_vector_t*), 64, 0);
	int enough = 64 / model->count;
	int* order = (int*)alloca(sizeof(int) * model->count);

	for (i = 0; i < model->count; i++)
		order[i] = i;

	gsl_ran_shuffle(rng, order, model->count, sizeof(int));

	// 按组件搜索
	for (i = 0; i < model->count; i++)
	{
		ccv_dpm_root_classifier_t* root_classifier = model->root + order[i];
		double scale_x = 1.0;
		double scale_y = 1.0;

		// 按尺度空间搜索
		for (j = next; j < scale_upto + next * 2; j++)
		{
			ccv_dense_matrix_t* root_feature = 0;
			ccv_dense_matrix_t* part_feature[CCV_DPM_PART_MAX];
			ccv_dense_matrix_t* dx[CCV_DPM_PART_MAX];
			ccv_dense_matrix_t* dy[CCV_DPM_PART_MAX];
			_ccv_dpm_compute_score(root_classifier, pyr[j], pyr[j - next], &root_feature, part_feature, dx, dy);
			int rwh = (root_classifier->root.w->rows - 1) / 2, rww = (root_classifier->root.w->cols - 1) / 2;
			int rwh_1 = root_classifier->root.w->rows / 2, rww_1 = root_classifier->root.w->cols / 2;
			float* f_ptr = (float*)ccv_get_dense_matrix_cell_by(CCV_32F | CCV_C1, root_feature, rwh, 0, 0);

			// 按根w的高来搜索	
			for (y = rwh; y < root_feature->rows - rwh_1; y++)
			{
				// 按根w的宽来搜索	
				for (x = rww; x < root_feature->cols - rww_1; x++)
				{
					if (f_ptr[x] + root_classifier->beta > threshold)
					{
						// 初始化特征向量v
						// initialize v
						ccv_dpm_feature_vector_t* v = (ccv_dpm_feature_vector_t*)ccmalloc(sizeof(ccv_dpm_feature_vector_t));
						_ccv_dpm_initialize_feature_vector_on_pattern(v, root_classifier, order[i]);

						// 收集特征向量
						_ccv_dpm_collect_feature_vector(v, f_ptr[x] + root_classifier->beta, x, y, pyr[j], pyr[j - next], dx, dy);

						// 记录收集到特征向量的尺度空间信息
						v->scale_x = scale_x;
						v->scale_y = scale_y;

						// 将收集到的特征向量压入堆栈av
						ccv_array_push(av, &v);

						if (av->rnum >= enough * (i + 1))
							break;
					}
				}
				
				f_ptr += root_feature->cols;

				if (av->rnum >= enough * (i + 1))
					break;
			}

			// 按部件个数释放部件特征向量和位移
			for (k = 0; k < root_classifier->count; k++)
			{
				ccv_matrix_free(part_feature[k]);
				ccv_matrix_free(dx[k]);
				ccv_matrix_free(dy[k]);
			}
			
			ccv_matrix_free(root_feature);
			scale_x *= scale;
			scale_y *= scale;
			
			if (av->rnum >= enough * (i + 1))
				break;
			
		}
	}
	
	for (i = 0; i < scale_upto + next * 2; i++)
		ccv_matrix_free(pyr[i]);
	
	return av;
}

static void 
_ccv_dpm_collect_from_background(ccv_array_t* av, 
								 gsl_rng* rng, 
								 char** bgfiles, 
								 int bgnum, 
								 ccv_dpm_mixture_model_t* model, 
								 ccv_dpm_new_param_t params, 
								 float threshold)
{
	int i, j;
	int* order = (int*)ccmalloc(sizeof(int) * bgnum);
	
	for (i = 0; i < bgnum; i++)
		order[i] = i;
	
	gsl_ran_shuffle(rng, order, bgnum, sizeof(int));

	// 按背景图的数量循环
	// 8: for j := 1 to m do
	for (i = 0; i < bgnum; i++)
	{
		FLUSH(CCV_CLI_INFO, " - collecting negative examples -- (%d%%)", av->rnum * 100 / params.negative_cache_size);
		ccv_dense_matrix_t* image = 0;

		// 将当前背景图读到image里
		ccv_read(bgfiles[order[i]], &image, (params.grayscale ? CCV_IO_GRAY : 0) | CCV_IO_ANY_FILE);

		// 将当前背景图里的所有负样本收集到at里面
		// 10: Add detect-all(Beta, Jj, -(1 + Delta)) to Fn
		ccv_array_t* at = _ccv_dpm_collect_all(rng, image, model, params.detector, threshold);

		if (at)
		{
			// 按负样本的数量循环
			for (j = 0; j < at->rnum; j++)
			{
				// 将当前背景样本集里的每个负样本压栈到av里面
				ccv_array_push(av, ccv_array_get(at, j));
			}
			
			ccv_array_free(at);
		}

		// 释放当前背景图
		ccv_matrix_free(image);

		// 如果负样本数量超过了负样本缓冲区大小则退出收集
		// 9: if |Fn >= memory-limit| then break 
		if (av->rnum >= params.negative_cache_size)
			break;
	}

	// 11: end
	ccfree(order);
}

/*
7 后处理
7.1 包围盒预测
	目标检测系统想要得到的输出并不是完全统一的。PASCAL挑战赛的目的是预测目标的
包围盒(BoundingBox)。我们之前的论文[17]是根据根滤波器的位置产生包围盒。但我们的
模型还能定位每个部件滤波器的位置。此外，部件滤波器的定位精度要大于根滤波器。很
明显我们最初的方法忽略了使用多尺度可变形部件模型所获得的潜在有价值信息。
    在当前的系统中，我们使用目标假设的完全配置，z = (p0,..., pn)，来预测目标的
包围盒。这是通过一个将特征向量g(z)映射为包围盒左上角点(x1,y1)和右下角点(x2,y2)
的函数来实现的。对于一个含n个部件的模型，g(z)是一个2n+3维的向量，包含以像素为单
位的根滤波器宽度(指出尺度信息)和每个滤波器在图像左上角点的位置坐标。
    PASCAL训练集中的每个目标都用包围盒进行了标注。训练完模型后，用检测器在每个
实例上的输出根据g(z)训练4个分别预测x1,y1,x2,y2的线性函数，这是通过线性最小二乘
回归来训练的，对于混合模型中的每个组件是独立的。
    图7是汽车检测中包围盒预测的一个例子。这种简单方法在PASCAL数据集的某些类别上
可以产生较小但值得注意的性能提升(见第8节)。

typedef struct   
{  
  size_t size1;//矩阵的行数  
  size_t size2;//矩阵的列数  
  size_t tda;//矩阵的实际列数  
  double * data;//实际上就是block->data  
  gsl_block * block;//矩阵相应的数据块  
  int owner;//所有者标识符  
} gsl_matrix; 

int gsl_multifit_linear (const gsl_matrix * X, const gsl_vector * y, gsl_vector * c, gsl_matrix * cov, double * chisq, gsl_multifit_linear_workspace * work)

//x是自变量 注意这儿是矩阵形式 每一行代表一个自变量的输入向量
//y是因变量  每一行代表相应x输入的结果
//c是结果 大小等于x的列向量的长度
//cov是系数的variance-covariance矩阵 是M*M (M是x的列长）
//chisq是残差
//work是工作空间 长宽与x的长宽相同
*/

static void 
_ccv_dpm_initialize_root_rectangle_estimator(ccv_dpm_mixture_model_t* model, 
											 char** posfiles, 
											 ccv_rect_t* bboxes, 
											 int posnum, 
											 ccv_dpm_new_param_t params)
{
	// 定义已处理的正样本计数c
	int i, j, k, c;

	// 给正样本向量分配空间
	ccv_dpm_feature_vector_t** posv = (ccv_dpm_feature_vector_t**)ccmalloc(sizeof(ccv_dpm_feature_vector_t*) * posnum);

	// 定义每个模型所对应的样本计数
	int* num_per_model = (int*)alloca(sizeof(int) * model->count);
	memset(num_per_model, 0, sizeof(int) * model->count);
	FLUSH(CCV_CLI_INFO, " - collecting responses from positive examples : 0%%");

	for (i = 0; i < posnum; i++)
	{
		FLUSH(CCV_CLI_INFO, " - collecting responses from positive examples : %d%%", i * 100 / posnum);
		ccv_dense_matrix_t* image = 0;

		// 将正样本文件读到输出图像列表image里面
		// 正样本文件,输出图像,类型
		ccv_read(posfiles[i], &image, (params.grayscale ? CCV_IO_GRAY : 0) | CCV_IO_ANY_FILE);

		// 从正样本图像集image里面找到离分类面最远的正样本集
		posv[i] = _ccv_dpm_collect_best(image, model, bboxes[i], params.include_overlap, params.detector);

		if (posv[i])
		{
			// 每个模型所对应的样本计数+1
			++num_per_model[posv[i]->id];
		}
		
		ccv_matrix_free(image);
	}

	// 这将预测新的x, y和scale漂移
	// this will estimate new x, y, and scale
	PRINT(CCV_CLI_INFO, "\n - linear regression for x, y, and scale drifting\n");

	for (i = 0; i < model->count; i++)
	{
		// 获取当前根分类器
		ccv_dpm_root_classifier_t* root_classifier = model->root + i;

		// 分配坐标矩阵空间 n1 * n2
		gsl_matrix* X = gsl_matrix_alloc(num_per_model[i], root_classifier->count * 2 + 1);
		gsl_vector* y[3];

		// 分配包围盒中心点归一化后的坐标矩阵
		y[0] = gsl_vector_alloc(num_per_model[i]);
		y[1] = gsl_vector_alloc(num_per_model[i]);

		// 分配归一化后的包围盒面积矩阵
		y[2] = gsl_vector_alloc(num_per_model[i]);
		gsl_vector* z = gsl_vector_alloc(root_classifier->count * 2 + 1);
		gsl_matrix* cov = gsl_matrix_alloc(root_classifier->count * 2 + 1, root_classifier->count * 2 + 1);;
		c = 0;
		
		for (j = 0; j < posnum; j++)
		{
			// 获取当前正样本向量v	
			ccv_dpm_feature_vector_t* v = posv[j];

			if (v && v->id == i)
			{
				// 设置第i个模型对应的坐标矩阵中的第i+1行，j+1列的元素为1.0
				gsl_matrix_set(X, c, 0, 1.0);

				for (k = 0; k < v->count; k++)
				{
					// 设置部件k的坐标到X
					gsl_matrix_set(X, c, k * 2 + 1, v->part[k].dx);
					gsl_matrix_set(X, c, k * 2 + 2, v->part[k].dy);
				}

				// 获取第j个正样本包围盒矩形到bbox
				ccv_rect_t bbox = bboxes[j];

				// y[0],y[1]包围盒中心点归一化后的坐标，y[2]归一化后的包围盒面积
				// 对vector赋值, vector名称, 分量序号, 值
				gsl_vector_set(y[0], 
							   c, 
							   (bbox.x + bbox.width * 0.5) / (v->scale_x * CCV_DPM_WINDOW_SIZE) 
							    - v->x);
				gsl_vector_set(y[1], 
							   c, 
							   (bbox.y + bbox.height * 0.5) / (v->scale_y * CCV_DPM_WINDOW_SIZE)
							    - v->y);
				gsl_vector_set(y[2], 
							   c, 
							   sqrt((bbox.width * bbox.height) 
							    / (root_classifier->root.w->rows * v->scale_x 
							    * CCV_DPM_WINDOW_SIZE 
							    * root_classifier->root.w->cols * v->scale_y 
							    * CCV_DPM_WINDOW_SIZE)) - 1.0);

				// 已处理的正样本计数+1
				++c;
			}
		}

		// 申请工作空间
		gsl_multifit_linear_workspace* workspace 
			= gsl_multifit_linear_alloc(num_per_model[i], 
										root_classifier->count * 2 + 1);
		double chisq;
		
		for (j = 0; j < 3; j++)
		{
			// 自变量 因变量 结果 协方差矩阵 残差 工作空间(长宽与x的长宽相同)
			gsl_multifit_linear(X, y[j], z, cov, &chisq, workspace);

			// 获取z(j0)到根分类器的alpha[j]
			root_classifier->alpha[j] = 
				params.discard_estimating_constant ? 0 : gsl_vector_get(z, 0);

			for (k = 0; k < root_classifier->count; k++)
			{
				// 获取第k个部件分类器指针
				ccv_dpm_part_classifier_t* part_classifier = root_classifier->part + k;

				// 获取拟合过后的系数z[]到部件分类器的alpha[j * 2],alpha[j * 2 + 1]
				part_classifier->alpha[j * 2] = gsl_vector_get(z, k * 2 + 1);
				part_classifier->alpha[j * 2 + 1] = gsl_vector_get(z, k * 2 + 2);
			}
		}
		
		gsl_multifit_linear_free(workspace);
		gsl_matrix_free(cov);
		gsl_vector_free(z);
		gsl_vector_free(y[0]);
		gsl_vector_free(y[1]);
		gsl_vector_free(y[2]);
		gsl_matrix_free(X);
	}

	// 释放正样本集
	for (i = 0; i < posnum; i++)
		if (posv[i])
			_ccv_dpm_feature_vector_free(posv[i]);
		
	ccfree(posv);
}

static void 
_ccv_dpm_regularize_mixture_model(ccv_dpm_mixture_model_t* model, 
								  int i, 
								  double regz)
{
	int k;
	ccv_dpm_root_classifier_t* root_classifier = model->root + i;
	int ch = CCV_GET_CHANNEL(root_classifier->root.w->type);
	ccv_make_matrix_mutable(root_classifier->root.w);
	float *wptr = root_classifier->root.w->data.f32;

	// 按根分类器w的元素个数循环
	// regz == 1.0 - pow(1.0 - alpha / (double)((pos_prog[j] + neg_prog[j]) * (!!symmetric + 1)), 
	for (i = 0; i < root_classifier->root.w->rows * root_classifier->root.w->cols * ch; i++)
	{
		// 规则化根分类器的w
		wptr[i] -= regz * wptr[i];
	}

	ccv_make_matrix_immutable(root_classifier->root.w);
	root_classifier->beta -= regz * root_classifier->beta;

	// 按根分类器的个数循环
	for (k = 0; k < root_classifier->count; k++)
	{
		ccv_dpm_part_classifier_t* part_classifier = root_classifier->part + k;
		ccv_make_matrix_mutable(part_classifier->w);
		wptr = part_classifier->w->data.f32;

		// 按部件分类器w的元素个数循环
		for (i = 0; i < part_classifier->w->rows * part_classifier->w->cols * ch; i++)
		{
			// 规则化部件分类器的w
			wptr[i] -= regz * wptr[i];
		}

		ccv_make_matrix_immutable(part_classifier->w);

		// 规则化部件分类器的偏移向量
		part_classifier->dx -= regz * part_classifier->dx;
		part_classifier->dxx -= regz * part_classifier->dxx;
		part_classifier->dy -= regz * part_classifier->dy;
		part_classifier->dyy -= regz * part_classifier->dyy;
		part_classifier->dxx = ccv_max(0.01, part_classifier->dxx);
		part_classifier->dyy = ccv_max(0.01, part_classifier->dyy);
	}
}

/*
随机梯度下降法优化流程
1. Let AlphaT be the learning for iteration t
2. Let i be a random example
3. Let Zi = argmax (Beta dp Phi(Xi, z))
4. If Yi * Fbeta(Xi) = Yi * (Beta dp Phi(Xi, Zi)) >= 1, set Beta := Beta - AlphaT * Beta
5. Else Beta := Beta - AlphaT * (Beta - Cn * Yi * Phi(Xi, Zi))
*/
static void _ccv_dpm_stochastic_gradient_descent(ccv_dpm_mixture_model_t* model, 
												 ccv_dpm_feature_vector_t* v, 
												 double y, 
												 double alpha, 
												 double Cn, 
												 int symmetric)
{
	if (v->id < 0 || v->id >= model->count)
		return;
	
	ccv_dpm_root_classifier_t* root_classifier = model->root + v->id;
	int i, j, k, c, ch = CCV_GET_CHANNEL(v->root.w->type);
	assert(ch == 31);
	assert(v->root.w->rows == root_classifier->root.w->rows && v->root.w->cols == root_classifier->root.w->cols);

	// 获取特征向量的根w指针
	float *vptr = v->root.w->data.f32;
	ccv_make_matrix_mutable(root_classifier->root.w);

	// 获取根分类器的根w指针
	float *wptr = root_classifier->root.w->data.f32;

	// 如果对称
	if (symmetric)
	{
		for (i = 0; i < v->root.w->rows; i++)
		{
			for (j = 0; j < v->root.w->cols; j++)
			{
				for (c = 0; c < ch; c++)
				{
					wptr[j * ch + c] += alpha * y * Cn * vptr[j * ch + c];

					// 计算y轴的对称点
					wptr[j * ch + c] += 
						alpha * y * Cn * vptr[(v->root.w->cols - 1 - j) * ch
					  + _ccv_dpm_sym_lut[c]];
				}
			}
			
			vptr += v->root.w->cols * ch;
			wptr += root_classifier->root.w->cols * ch;
		}

		/* 在训练的时候对部分部件进行打标签，用他们求beta,然后用beta再来找潜在
		部件，因此使用coordinatedescent迭代求解，再一次遇到这个求解方法。有了部
		件和打分，就是寻找根部件和其他部件的结合匹配最优问题，可以使用动态规划，
		但很慢*/
		root_classifier->beta += alpha * y * Cn * 2.0;
	} 
	else // 如果不对称
	{
		// 按特征向量的元素数循环
		for (i = 0; i < v->root.w->rows * v->root.w->cols * ch; i++)
		{
			// ? 5. Beta := Beta - AlphaT * (Beta - Cn * Yi * Phi(Xi, Zi))
			wptr[i] += alpha * y * Cn * vptr[i];
		}
		
		root_classifier->beta += alpha * y * Cn;
	}
	
	ccv_make_matrix_immutable(root_classifier->root.w);
	assert(v->count == root_classifier->count);

	// 按部件分类器的个数循环
	for (k = 0; k < v->count; k++)
	{
		// 获取第k个部件分类器指针
		ccv_dpm_part_classifier_t* part_classifier = root_classifier->part + k;
		ccv_make_matrix_mutable(part_classifier->w);

		// 获取第k个部件特征向量指针
		ccv_dpm_part_classifier_t* part_vector = v->part + k;
		assert(part_vector->w->rows == part_classifier->w->rows && part_vector->w->cols == part_classifier->w->cols);
		part_classifier->dx -= alpha * y * Cn * part_vector->dx;
		part_classifier->dxx -= alpha * y * Cn * part_vector->dxx;
		part_classifier->dxx = ccv_max(part_classifier->dxx, 0.01);
		part_classifier->dy -= alpha * y * Cn * part_vector->dy;
		part_classifier->dyy -= alpha * y * Cn * part_vector->dyy;
		part_classifier->dyy = ccv_max(part_classifier->dyy, 0.01);

		// 获取部件特征向量的w指针
		vptr = part_vector->w->data.f32;

		// 获取部件分类器的w指针
		wptr = part_classifier->w->data.f32;

		// 如果对称
		if (symmetric)
		{
			// 对于特征对称的所有样本做2x汇聚
			// 2x converge on everything for symmetric feature
			if (part_classifier->counterpart == -1)
			{
				part_classifier->dx += alpha * y * Cn * part_vector->dx; /* flip the sign on x-axis (symmetric) */
				part_classifier->dxx -= alpha * y * Cn * part_vector->dxx;
				part_classifier->dxx = ccv_max(part_classifier->dxx, 0.01);
				part_classifier->dy -= alpha * y * Cn * part_vector->dy;
				part_classifier->dyy -= alpha * y * Cn * part_vector->dyy;
				part_classifier->dyy = ccv_max(part_classifier->dyy, 0.01);

				for (i = 0; i < part_vector->w->rows; i++)
				{
					for (j = 0; j < part_vector->w->cols; j++)
					{
						for (c = 0; c < ch; c++)
						{
							wptr[j * ch + c] += alpha * y * Cn * vptr[j * ch + c];

							// 计算y轴的对称点
							wptr[j * ch + c] += 
								alpha * y * Cn * vptr[(part_vector->w->cols - 1 - j) * ch 
							  + _ccv_dpm_sym_lut[c]];
						}
					}
						
					vptr += part_vector->w->cols * ch;
					wptr += part_classifier->w->cols * ch;
				}
			} 
			else // 对于特征不对称的则计算另一个部件分类器other_part_classifier
			{
				ccv_dpm_part_classifier_t* other_part_classifier = 
					root_classifier->part + part_classifier->counterpart;
				
				assert(part_vector->w->rows == other_part_classifier->w->rows && part_vector->w->cols == other_part_classifier->w->cols);

				other_part_classifier->dx += alpha * y * Cn * part_vector->dx; /* flip the sign on x-axis (symmetric) */
				other_part_classifier->dxx -= alpha * y * Cn * part_vector->dxx;
				other_part_classifier->dxx = ccv_max(other_part_classifier->dxx, 0.01);
				other_part_classifier->dy -= alpha * y * Cn * part_vector->dy;
				other_part_classifier->dyy -= alpha * y * Cn * part_vector->dyy;
				other_part_classifier->dyy = ccv_max(other_part_classifier->dyy, 0.01);

				// ? 5. Beta := Beta - AlphaT * (Beta - Cn * Yi * Phi(Xi, Zi))
				for (i = 0; i < part_vector->w->rows; i++)
				{
					for (j = 0; j < part_vector->w->cols * ch; j++)
					{
						wptr[j] += alpha * y * Cn * vptr[j];
					}
					
					vptr += part_vector->w->cols * ch;
					wptr += part_classifier->w->cols * ch;
				}
				
				vptr = part_vector->w->data.f32;
				wptr = other_part_classifier->w->data.f32;
				
				for (i = 0; i < part_vector->w->rows; i++)
				{
					for (j = 0; j < part_vector->w->cols; j++)
					{
						for (c = 0; c < ch; c++)
						{
							wptr[j * ch + c] += 
								alpha * y * Cn * vptr[(part_vector->w->cols - 1 - j) * ch 
							  + _ccv_dpm_sym_lut[c]];
						}
					}
					
					vptr += part_vector->w->cols * ch;
					wptr += other_part_classifier->w->cols * ch;
				}
			}
		} 
		else // 如果不对称
		{
			for (i = 0; i < part_vector->w->rows * part_vector->w->cols * ch; i++)
			{
				wptr[i] += alpha * y * Cn * vptr[i];
			}
		}
		
		ccv_make_matrix_immutable(part_classifier->w);
	}
}

static void _ccv_dpm_write_gradient_descent_progress(int i, int j, const char* dir)
{
	char swpfile[1024];
	sprintf(swpfile, "%s.swp", dir);
	FILE* w = fopen(swpfile, "w+");
	if (!w)
		return;
	fprintf(w, "%d %d\n", i, j);
	fclose(w);
	rename(swpfile, dir);
}

static void _ccv_dpm_read_gradient_descent_progress(int* i, int* j, const char* dir)
{
	FILE* r = fopen(dir, "r");
	if (!r)
		return;
	fscanf(r, "%d %d", i, j);
	fclose(r);
}

static void _ccv_dpm_write_feature_vector(FILE* w, ccv_dpm_feature_vector_t* v)
{
	int j, x, y, ch;
	
	if (v)
	{
		fprintf(w, "%d %d %d\n", v->id, v->root.w->rows, v->root.w->cols);
		ch = CCV_GET_CHANNEL(v->root.w->type);
		for (y = 0; y < v->root.w->rows; y++)
		{
			for (x = 0; x < v->root.w->cols * ch; x++)
				fprintf(w, "%a ", v->root.w->data.f32[y * v->root.w->cols * ch + x]);
			fprintf(w, "\n");
		}
		fprintf(w, "%d %a\n", v->count, v->score);
		for (j = 0; j < v->count; j++)
		{
			ccv_dpm_part_classifier_t* part_classifier = v->part + j;
			fprintf(w, "%la %la %la %la\n", part_classifier->dx, part_classifier->dy, part_classifier->dxx, part_classifier->dyy);
			fprintf(w, "%d %d %d\n", part_classifier->x, part_classifier->y, part_classifier->z);
			fprintf(w, "%d %d\n", part_classifier->w->rows, part_classifier->w->cols);
			ch = CCV_GET_CHANNEL(part_classifier->w->type);
			for (y = 0; y < part_classifier->w->rows; y++)
			{
				for (x = 0; x < part_classifier->w->cols * ch; x++)
					fprintf(w, "%a ", part_classifier->w->data.f32[y * part_classifier->w->cols * ch + x]);
				fprintf(w, "\n");
			}
		}
	} else {
		fprintf(w, "0 0 0\n");
	}
}

static ccv_dpm_feature_vector_t* _ccv_dpm_read_feature_vector(FILE* r)
{
	int id, rows, cols, j, k;
	fscanf(r, "%d %d %d", &id, &rows, &cols);
	if (rows == 0 && cols == 0)
		return 0;
	ccv_dpm_feature_vector_t* v = (ccv_dpm_feature_vector_t*)ccmalloc(sizeof(ccv_dpm_feature_vector_t));
	v->id = id;
	v->root.w = ccv_dense_matrix_new(rows, cols, CCV_32F | 31, 0, 0);
	for (j = 0; j < rows * cols * 31; j++)
		fscanf(r, "%f", &v->root.w->data.f32[j]);
	fscanf(r, "%d %f", &v->count, &v->score);
	v->part = (ccv_dpm_part_classifier_t*)ccmalloc(sizeof(ccv_dpm_part_classifier_t) * v->count);
	for (j = 0; j < v->count; j++)
	{
		ccv_dpm_part_classifier_t* part_classifier = v->part + j;
		fscanf(r, "%lf %lf %lf %lf", &part_classifier->dx, &part_classifier->dy, &part_classifier->dxx, &part_classifier->dyy);
		fscanf(r, "%d %d %d", &part_classifier->x, &part_classifier->y, &part_classifier->z);
		fscanf(r, "%d %d", &rows, &cols);
		part_classifier->w = ccv_dense_matrix_new(rows, cols, CCV_32F | 31, 0, 0);
		for (k = 0; k < rows * cols * 31; k++)
			fscanf(r, "%f", &part_classifier->w->data.f32[k]);
	}
	return v;
}

static void _ccv_dpm_write_positive_feature_vectors(ccv_dpm_feature_vector_t** vs, int n, const char* dir)
{
	FILE* w = fopen(dir, "w+");
	
	if (!w)
		return;

	fprintf(w, "%d\n", n);
	int i;

	for (i = 0; i < n; i++)
		_ccv_dpm_write_feature_vector(w, vs[i]);

	fclose(w);
}

static int _ccv_dpm_read_positive_feature_vectors(ccv_dpm_feature_vector_t** vs, int _n, const char* dir)
{
	FILE* r = fopen(dir, "r");
	
	if (!r)
		return -1;

	int n;
	fscanf(r, "%d", &n);
	assert(n == _n);
	int i;

	for (i = 0; i < n; i++)
		vs[i] = _ccv_dpm_read_feature_vector(r);

	fclose(r);

	return 0;
}

static void _ccv_dpm_write_negative_feature_vectors(ccv_array_t* negv, int negative_cache_size, const char* dir)
{
	FILE* w = fopen(dir, "w+");
	
	if (!w)
		return;
	
	fprintf(w, "%d %d\n", negative_cache_size, negv->rnum);
	int i;
	
	for (i = 0; i < negv->rnum; i++)
	{
		ccv_dpm_feature_vector_t* v = *(ccv_dpm_feature_vector_t**)ccv_array_get(negv, i);
		_ccv_dpm_write_feature_vector(w, v);
	}
	
	fclose(w);
}

static int _ccv_dpm_read_negative_feature_vectors(ccv_array_t** _negv, int _negative_cache_size, const char* dir)
{
	FILE* r = fopen(dir, "r");
	
	if (!r)
		return -1;

	int negative_cache_size, negnum;
	fscanf(r, "%d %d", &negative_cache_size, &negnum);
	assert(negative_cache_size == _negative_cache_size);
	ccv_array_t* negv = *_negv = ccv_array_new(sizeof(ccv_dpm_feature_vector_t*), negnum, 0);
	int i;

	for (i = 0; i < negnum; i++)
	{
		ccv_dpm_feature_vector_t* v = _ccv_dpm_read_feature_vector(r);
		assert(v);
		ccv_array_push(negv, &v);
	}
	
	fclose(r);
	return 0;
}

static void _ccv_dpm_adjust_model_constant(ccv_dpm_mixture_model_t* model, int k, ccv_dpm_feature_vector_t** posv, int posnum, double percentile)
{
	int i, j;
	double* scores = (double*)ccmalloc(posnum * sizeof(double));
	j = 0;
	
	for (i = 0; i < posnum; i++)
		if (posv[i] && posv[i]->id == k)
		{
			scores[j] = _ccv_dpm_vector_score(model, posv[i]);
			j++;
		}
		
	_ccv_dpm_score_qsort(scores, j, 0);
	float adjust = scores[ccv_clamp((int)(percentile * j), 0, j - 1)];

	// 调整百分率
	// adjust to percentile
	model->root[k].beta -= adjust;
	PRINT(CCV_CLI_INFO, " - tune model %d constant for %f\n", k + 1, -adjust);
	ccfree(scores);
}

static void _ccv_dpm_check_params(ccv_dpm_new_param_t params)
{
	assert(params.components > 0);
	assert(params.parts > 0);
	assert(params.grayscale == 0 || params.grayscale == 1);
	assert(params.symmetric == 0 || params.symmetric == 1);
	assert(params.min_area > 100);
	assert(params.max_area > params.min_area);
	assert(params.iterations >= 0);
	assert(params.data_minings >= 0);
	assert(params.relabels >= 0);
	assert(params.negative_cache_size > 0);
	assert(params.include_overlap > 0.1);
	assert(params.alpha > 0 && params.alpha < 1);
	assert(params.alpha_ratio > 0 && params.alpha_ratio < 1);
	assert(params.C > 0);
	assert(params.balance > 0);
	assert(params.percentile_breakdown > 0 && params.percentile_breakdown <= 1);
	assert(params.detector.interval > 0);
}

#define MINI_BATCH (10)
#define REGQ (100)

/*
L-SVM中的优化问题可以使用坐标下降法求解：
1）保持W'固定，在正样本集合内优化隐藏变量q, qi = argmax[qi∈Q(xi)] β* Φ(W', q)  
2）根据步骤一的结果保持{qi}固定,公式(3-9)变为凸函数,可以转换为凸二次规划问题求解。
3）循环以上两个步骤，直到最后的结果收敛或者满足给定的条件为止。



让Zp假设一个训练集里所有正例的隐形变量，LSVM的优化过程如下所示：

1) 重新标记训练集的图像：在确定β的情况下，对每个训练集图像找的得分最高的位置：
argmax[z∈Z(xi)] β* Φ(xi, z)          (3-11) 

2) 重新优化模型参数：
LD(β) = 1 / 2 * ||β|| ^ 2 + C∑max(0, 1 - yi * fβ(xi))  (3-12) 

两个步骤循环交替进行，模型的参数将一步步地得到优化，最终收敛到最优参数上。
     
对于上述步骤的第二步，我们采用随机梯度下降的方法来进行优化，
  
让zi(β) = β,然后fβ(xi) = β* φ(xi, zi(β))，我们计算LSVM目标函数的梯度如下所示：
              
▽LD(β) = β+ C∑[i=1~n] h(β, xi, yi)      (3-13) 
                 
h(β,xi,yi) = -yi * (xi,zi(β))     (3-14) 
 
在h(β,xi,yi)中,当yi * fβ(xi) >= 1时取上值，否则取下值，∑[i=1~n] h(β, xi, yi)          
使用n * h(β,xi,yi)来近似，综上所述循环算法如下所示，
 
1) 让αi表示梯度下降的步长。 
2) 让i为一个随机数。
3) 让zi(β) = argmax[z∈Z(xi)] β* Φ(xi, z)  。 
4) 如果yi * fβ( xi ) >= 1，β = β - αi * β。
5) 否则β = β - αi * (β – Cn * yi * φ(xi, zi))
*/
static ccv_dpm_mixture_model_t* 
_ccv_dpm_optimize_root_mixture_model(gsl_rng* rng, 
									 ccv_dpm_mixture_model_t* model, 
									 ccv_array_t** posex, 
									 ccv_array_t** negex, 
									 int relabels, 
									 double balance, 
									 double C, 
									 double previous_alpha, 
									 double alpha_ratio, 
									 int iterations, 
									 int symmetric)
{
	int i, j, k, t, c;
	
	for (i = 0; i < model->count - 1; i++)
		assert(posex[i]->rnum == posex[i + 1]->rnum && negex[i]->rnum == negex[i + 1]->rnum);
	
	int posnum = posex[0]->rnum;
	int negnum = negex[0]->rnum;

	// 定义所有样本的label数组
	int* label = (int*)ccmalloc(sizeof(int) * (posnum + negnum));
	int* order = (int*)ccmalloc(sizeof(int) * (posnum + negnum));
	double previous_positive_loss = 0, 
		   previous_negative_loss = 0, 
		   positive_loss = 0, 
		   negative_loss = 0, 
		   loss = 0;

	// C in SVM. Cn
	double regz_rate = C;

	// 根分类器需要的重标记过程数root_relabels = 20,c在循环体中没有用到
	for (c = 0; c < relabels; c++)
	{
		int* pos_prog = (int*)alloca(sizeof(int) * model->count);
		memset(pos_prog, 0, sizeof(int) * model->count);

		// 计算每一个正样本在所有模型上的最好响应分数和组件号
		// 1）保持W'固定，在正样本集合内优化隐藏变量q, qi = argmax[qi∈Q(xi)] β* Φ(W', q)  
		// 1) 重新标记训练集的图像：在确定β的情况下，对每个训练集图像找的得分最高的位置：
		// argmax[z∈Z(xi)] β* Φ(xi, z) 
		for (i = 0; i < posnum; i++)
		{
			int best = -1;
			double best_score = -DBL_MAX;

			// 按模型个数循环
			for (k = 0; k < model->count; k++)
			{
				// 获取第k个模型的第i个正样本到v	
				ccv_dpm_feature_vector_t* v 
					= (ccv_dpm_feature_vector_t*)ccv_array_get(posex[k], i);

				if (v->root.w == 0)
					continue;

				// 最小批方法的损失(在模型上计算)
				double score = _ccv_dpm_vector_score(model, v); // the loss for mini-batch method (computed on model)

				// 获取正样本的最好得分best_score和最好组件号best
				if (score > best_score)
				{
					best = k;
					best_score = score;
				}
			}

			// 将第i个正样本的label设置为最好组件号best
			label[i] = best;

			// 组件号对应的位置计数器+1
			if (best >= 0)
				++pos_prog[best];
		}
		
		PRINT(CCV_CLI_INFO, " - positive examples divided by components for root model optimizing : %d", pos_prog[0]);

		for (i = 1; i < model->count; i++)
			PRINT(CCV_CLI_INFO, ", %d", pos_prog[i]);

		PRINT(CCV_CLI_INFO, "\n");
		int* neg_prog = (int*)alloca(sizeof(int) * model->count);
		memset(neg_prog, 0, sizeof(int) * model->count);

		// 按负样本个数循环
		for (i = 0; i < negnum; i++)
		{
			// 在模型个数范围内取随机数作为负样本的最好组件号
			int best = gsl_rng_uniform_int(rng, model->count);

			// 设置负样本的label为最好组件号best
			label[i + posnum] = best;

			// 组件号对应的位置计数器+1
			++neg_prog[best];
		}
		
		PRINT(CCV_CLI_INFO, " - negative examples divided by components for root model optimizing : %d", neg_prog[0]);

		for (i = 1; i < model->count; i++)
			PRINT(CCV_CLI_INFO, ", %d", neg_prog[i]);
		PRINT(CCV_CLI_INFO, "\n");
		
		ccv_dpm_mixture_model_t* _model;

		// 2.1) 让αi表示梯度下降的步长。 
		/**< The step size for stochastic gradient descent. */
		double alpha = previous_alpha;
		previous_positive_loss = previous_negative_loss = 0;

		// 2）根据步骤一的结果保持{qi}固定,公式(3-9)变为凸函数,可以转换为凸二次规划问题求解。
		// 2) 重新优化模型参数:LD(β) = 1 / 2 * ||β|| ^ 2 + C∑max(0, 1 - yi * fβ(xi))
		// 500 1000 50000
		for (t = 0; t < iterations; t++)
		{
			for (i = 0; i < posnum + negnum; i++)
			{
				order[i] = i;
			}

			// 2.2) 让i为一个随机数。
			// 把order打乱成随机序列
			gsl_ran_shuffle(rng, order, posnum + negnum, sizeof(int));

			for (j = 0; j < model->count; j++)
			{
				// 计算正负样本权重
				double pos_weight = sqrt((double)neg_prog[j] / pos_prog[j] * balance); // positive weight
				double neg_weight = sqrt((double)pos_prog[j] / neg_prog[j] / balance); // negative weight
				_model = _ccv_dpm_model_copy(model);
				int l = 0;

				// 按正负样本总数循环
				for (i = 0; i < posnum + negnum; i++)
				{
					// 随机取一个样本号
					k = order[i];

					// 如果样本类别等于当前组件号
					if (label[k]  == j)
					{
						assert(label[k] < model->count);

						// 如果是正样本
						// 2.3) 让zi(β) = argmax[z∈Z(xi)] β* Φ(xi, z)
						if (k < posnum)
						{
							// 获取组件号为label[k]的第k个正样本特征向量v
							ccv_dpm_feature_vector_t* v = 
								(ccv_dpm_feature_vector_t*)ccv_array_get(posex[label[k]], k);

							assert(v->root.w);

							// 计算当前样本特征向量v的得分score
							double score = _ccv_dpm_vector_score(model, v); // the loss for mini-batch method (computed on model)

							assert(!isnan(score));
							assert(v->id == j);

							// 2.4) 如果yi * fβ( xi ) >= 1，β = β - αi * β。
							if (score <= 1)
							{
								// 优化更新根w,beta,部件w,dx,dy,dxx,dyy
								_ccv_dpm_stochastic_gradient_descent(_model, 
																	 v, 
																	 1, 
																	 alpha * pos_weight, 
																	 regz_rate, 
																	 symmetric);
							}
						} 
						else // 如果是负样本
						{
							// 获取组件号为label[k]的第k - posnum个负样本特征向量v
							ccv_dpm_feature_vector_t* v = 
								(ccv_dpm_feature_vector_t*)ccv_array_get(negex[label[k]], k - posnum);

							// 计算当前负样本特征向量v的得分score
							double score = _ccv_dpm_vector_score(model, v);
							assert(!isnan(score));
							assert(v->id == j);

							// 2.5) 否则β = β - αi * (β – Cn * yi * φ(xi, zi))
							if (score >= - 1)
							{
								// 优化更新根w,beta,部件w,dx,dy,dxx,dyy
								_ccv_dpm_stochastic_gradient_descent(_model, 
																	 v, 
																	 - 1, 
																	 alpha * neg_weight, 
																	 regz_rate, 
																	 symmetric);
							}
						}

						// 循环计数+1
						++l;

						// 每循环REGQ - 1次做一次规则化混合模型
						if (l % REGQ == REGQ - 1)
						{
							// 用第3个参数归一化根分类器的w和beta,部件分类器的w,dx,dy,dxx,dyy	
							_ccv_dpm_regularize_mixture_model(_model, 
															  j, 
															  1.0 - pow(1.0 - alpha / (double)((pos_prog[j] + neg_prog[j]) * (!!symmetric + 1)), REGQ));
						}

						// 每循环MINI_BATCH - 1次做一次释放
						if (l % MINI_BATCH == MINI_BATCH - 1)
						{
							// 释放部件分类器,根分类器
							// 模仿做事的最小批处理方法
							// mimicking mini-batch way of doing things
							_ccv_dpm_mixture_model_cleanup(model);
							
							ccfree(model);
							model = _model;
							_model = _ccv_dpm_model_copy(model);
						}
					}
				}

				// 按正负样本总数循环完后再做一次归一化
				_ccv_dpm_regularize_mixture_model(_model, 
												  j, 
												  1.0 - pow(1.0 - alpha / (double)((pos_prog[j] + neg_prog[j]) * (!!symmetric + 1)), 
												  (((pos_prog[j] + neg_prog[j]) % REGQ) + 1) % (REGQ + 1)));

				_ccv_dpm_mixture_model_cleanup(model);
				ccfree(model);
				model = _model;
			}
			
			// compute the loss
			positive_loss = negative_loss = loss = 0;
			int posvn = 0;

			// 计算正样本的加权铰链损失
			for (i = 0; i < posnum; i++)
			{
				// 如果样本标示为负则计算下一个正样本
				if (label[i] < 0)
					continue;
				
				assert(label[i] < model->count);

				// 获取当前正样本特征向量v
				ccv_dpm_feature_vector_t* v = (ccv_dpm_feature_vector_t*)ccv_array_get(posex[label[i]], i);

				if (v->root.w)
				{
					// 计算当前正样本特征向量v的得分score
					// score = yi * Fbata(xi)
					double score = _ccv_dpm_vector_score(model, v);
					assert(!isnan(score));

					// 计算当前正样本的铰链损失hinge_loss
					double hinge_loss = ccv_max(0, 1.0 - score);

					// 计算所有正样本的铰链损失之和positive_loss
					positive_loss += hinge_loss;

					// 计算正样本权重pos_weight
					double pos_weight = sqrt((double)neg_prog[v->id] / pos_prog[v->id] * balance); // positive weight

					// 计算加权铰链损失loss
					loss += pos_weight * hinge_loss;

					// 已经计算的有效正样本计数+1
					++posvn;
				}
			}

			// 计算负样本的加权铰链损失
			for (i = 0; i < negnum; i++)
			{
				// 如果样本标示为正则计算下一个正样本
				if (label[i + posnum] < 0)
					continue;
				
				assert(label[i + posnum] < model->count);

				// 获取当前负样本特征向量v
				ccv_dpm_feature_vector_t* v = (ccv_dpm_feature_vector_t*)ccv_array_get(negex[label[i + posnum]], i);

				// 计算当前负样本特征向量v的得分score
				double score = _ccv_dpm_vector_score(model, v);
				assert(!isnan(score));

				// 计算当前负样本的铰链损失hinge_loss
				double hinge_loss = ccv_max(0, 1.0 + score);

				// 计算所有负样本的铰链损失之和negative_loss
				negative_loss += hinge_loss;

				// 计算正样本权重neg_weight
				double neg_weight = sqrt((double)pos_prog[v->id] / neg_prog[v->id] / balance); // negative weight

				// 计算加权铰链损失loss
				loss += neg_weight * hinge_loss;
			}

			// 计算所有样本的平均铰链损失loss
			loss = loss / (posvn + negnum);

			// 计算正样本的平均铰链损失positive_loss
			positive_loss = positive_loss / posvn;

			// 计算负样本的平均铰链损失negative_loss
			negative_loss = negative_loss / negnum;
			FLUSH(CCV_CLI_INFO, " - with loss %.5lf (positive %.5lf, negative %.5f) at rate %.5lf %d | %d -- %d%%", loss, positive_loss, negative_loss, alpha, posvn, negnum, (t + 1) * 100 / iterations);

			// 检查已生成的根特征的对称性并打印输出
			// check symmetric property of generated root feature
			if (symmetric)
			{
				for (i = 0; i < model->count; i++)
				{
					ccv_dpm_root_classifier_t* root_classifier = model->root + i;
					_ccv_dpm_check_root_classifier_symmetry(root_classifier->root.w);
				}
			}

			// 如果正负样本的平均铰链损失都接近上一次的值则退出迭代
			if (fabs(previous_positive_loss - positive_loss) < 1e-5 &&
				fabs(previous_negative_loss - negative_loss) < 1e-5)
			{
				PRINT(CCV_CLI_INFO, "\n - aborting iteration at %d because we didn't gain much", t + 1);
				break;
			}

			// 记录正负样本的平均铰链损失
			previous_positive_loss = positive_loss;
			previous_negative_loss = negative_loss;

			// 每次迭代它将减少
			alpha *= alpha_ratio; // it will decrease with each iteration
		}

		PRINT(CCV_CLI_INFO, "\n");
	}
	
	ccfree(order);
	ccfree(label);
	
	return model;
}

// 用给定的正样本和背景图来创建一个新的DPM混合模型
/*
提下各阶段的工作，主要是论文中没有的Latent 变量分析：
Phase1:是传统的SVM训练过程，与HOG算法一致。作者是随机将正样本按照aspect ration
（长宽比）排序，然后很粗糙的均分为两半训练两个component的rootfilter。这两个
rootfilter的size也就直接由分到的pos examples决定了。后续取正样本时，直接将正样本
缩放成rootfilter的大小。
Phase2:是LSVM训练。Latent variables 有图像中正样本的实际位置包括空间位置（x,y）,
尺度位置level，以及component的类别c，即属于component1 还是属于 component 2。要训
练的参数为两个 rootfilter，offset（b）
Phase3:也是LSVM过程。
先提下子模型的添加。作者固定了每个component有6个partfilter，但实际上还会根据实际
情况减少。为了减少参数，partfilter都是对称的。partfilter在rootfilter中的锚点
（anchor location）在按最大energy选取partfilter的时候就已经固定下来了。
这阶段的Latent variables是最多的有：rootfilter（x,y,scale）,partfilters(x,y,scale)。
要训练的参数为 rootfilters, rootoffset, partfilters, defs(的偏移Cost)。

posfiles: An array of positive images.
bboxes: An array of bounding boxes for positive images.
posnum: Number of positive examples.
bgfiles: An array of background images.
bgnum: Number of background images.
negnum: Number of negative examples that is harvested from background images.
dir: The working directory to store/retrieve intermediate data.
params: A ccv_dpm_new_param_t structure that defines various aspects of the training function.
*/
void ccv_dpm_mixture_model_new(char** posfiles, 
							   ccv_rect_t* bboxes, 
							   int posnum, 
							   char** bgfiles, 
							   int bgnum, 
							   int negnum, 
							   const char* dir, 
							   ccv_dpm_new_param_t params)
{
	int t, d, c, i, j, k, p;
	_ccv_dpm_check_params(params);
	assert(params.negative_cache_size <= negnum && params.negative_cache_size > REGQ && params.negative_cache_size > MINI_BATCH);
	PRINT(CCV_CLI_INFO, "with %d positive examples and %d negative examples\n"
		   "negative examples are are going to be collected from %d background images\n",
		   posnum, negnum, bgnum);
	PRINT(CCV_CLI_INFO, "use symmetric property? %s\n", params.symmetric ? "yes" : "no");
	PRINT(CCV_CLI_INFO, "use color? %s\n", params.grayscale ? "no" : "yes");
	PRINT(CCV_CLI_INFO, "negative examples cache size : %d\n", params.negative_cache_size);
	PRINT(CCV_CLI_INFO, "%d components and %d parts\n", params.components, params.parts);
	PRINT(CCV_CLI_INFO, "expected %d root relabels, %d relabels, %d data minings and %d iterations\n", params.root_relabels, params.relabels, params.data_minings, params.iterations);
	PRINT(CCV_CLI_INFO, "include overlap : %lf\n"
						"alpha : %lf\n"
						"alpha decreasing ratio : %lf\n"
						"C : %lf\n"
						"balance ratio : %lf\n"
						"------------------------\n",
		   params.include_overlap, params.alpha, params.alpha_ratio, params.C, params.balance);

	gsl_rng_env_setup();
	gsl_rng* rng = gsl_rng_alloc(gsl_rng_default);
	gsl_rng_set(rng, *(unsigned long int*)&params);

	// 创建DPM模型
	ccv_dpm_mixture_model_t* model = (ccv_dpm_mixture_model_t*)ccmalloc(sizeof(ccv_dpm_mixture_model_t));
	memset(model, 0, sizeof(ccv_dpm_mixture_model_t));

	// 按正样本数目定义特征节点指针
	struct feature_node* fn = (struct feature_node*)ccmalloc(sizeof(struct feature_node) * posnum);

	for (i = 0; i < posnum; i++)
	{
		assert(bboxes[i].width > 0 && bboxes[i].height > 0);

		// 直接按照边界的长宽比，分为两半训练
		fn[i].value = (float)bboxes[i].width / (float)bboxes[i].height;
		fn[i].index = i;
	}
	char checkpoint[512];
	char initcheckpoint[512];
	sprintf(checkpoint, "%s/model", dir);
	sprintf(initcheckpoint, "%s/init.model", dir);

	// 按节点的value排序
	_ccv_dpm_aspect_qsort(fn, posnum, 0);
	double mean = 0;

	// 计算所有正样本节点长宽比的平均值mean
	for (i = 0; i < posnum; i++)
		mean += fn[i].value;
	mean /= posnum;

	// 计算所有正样本节点长宽比的均方差variance
	double variance = 0;
	for (i = 0; i < posnum; i++)
		variance += (fn[i].value - mean) * (fn[i].value - mean);
	variance /= posnum;
	
	PRINT(CCV_CLI_INFO, "global mean: %lf, & variance: %lf\ninterclass mean(variance):", mean, variance);

	// 定义每个组件对应的正样本个数数组mnum,按组件数量分配内存mnum
	int* mnum = (int*)alloca(sizeof(int) * params.components);
	int outnum = posnum, innum = 0;

	// 按组件数量循环 --model-component 1 
	for (i = 0; i < params.components; i++)
	{
		// 按components个数把正样本分成components部分
		mnum[i] = (int)((double)outnum / (double)(params.components - i) + 0.5);
		double mean = 0;

		// 计算该组件对应的部分正样本节点长宽比的平均值mean
		for (j = innum; j < innum + mnum[i]; j++)
			mean += fn[j].value;
		mean /= mnum[i];
		double variance = 0;

		// 计算该组件对应的部分正样本节点长宽比的均方差variance
		for (j = innum; j < innum + mnum[i]; j++)
			variance += (fn[j].value - mean) * (fn[j].value - mean);
		variance /= mnum[i];

		PRINT(CCV_CLI_INFO, " %lf(%lf)", mean, variance);
		outnum -= mnum[i];
		innum += mnum[i];
	}
	
	PRINT(CCV_CLI_INFO, "\n");
	int* areas = (int*)ccmalloc(sizeof(int) * posnum);

	// 计算各个正样本包围盒的面积
	for (i = 0; i < posnum; i++)
		areas[i] = bboxes[i].width * bboxes[i].height;

	// ?按升序排列面积
	_ccv_dpm_area_qsort(areas, posnum, 0);

	// 将前1/5正样本的最大面积饱和到min_area~max_area 3000~5000
	// 即使目标只有1/4的尺寸，也可以检测到它们(从2x图像开始检测)
	// so even the object is 1/4 in size, we can still detect them (in detection phase, we start at 2x image)
	int area = ccv_clamp(areas[(int)(posnum * 0.2 + 0.5)], params.min_area, params.max_area);
	ccfree(areas);
	innum = 0;
	_ccv_dpm_read_checkpoint(model, checkpoint);


	if (model->count <= 0)
	{
		// 用liblinear初始化根滤波器	
		/* initialize root mixture model with liblinear */
		model->count = params.components;
		model->root = (ccv_dpm_root_classifier_t*)ccmalloc(sizeof(ccv_dpm_root_classifier_t) * model->count);
		memset(model->root, 0, sizeof(ccv_dpm_root_classifier_t) * model->count);
	}
	
	PRINT(CCV_CLI_INFO, "computing root mixture model dimensions: ");
	fflush(stdout);
	int* poslabels = (int*)ccmalloc(sizeof(int) * posnum);
	int* rows = (int*)alloca(sizeof(int) * params.components);
	int* cols = (int*)alloca(sizeof(int) * params.components);
	
	for (i = 0; i < params.components; i++)
	{
		double aspect = 0;

		// 在一个组件内的正样本循环
		for (j = innum; j < innum + mnum[i]; j++)
		{
			aspect += fn[j].value;

			// 设置该组件对应的正样本标签为组件号i
			poslabels[fn[j].index] = i; // setup labels
		}
		
		// 计算该组件对应的正样本的平均长宽比aspect
		aspect /= mnum[i];

		// 根据前1/5正样本的最大长宽来计算DPM扫描块矩阵的行列
		// area前1/5正样本的最大面积，height == sqrtf(area / aspect)
		cols[i] = ccv_max((int)(sqrtf(area / aspect) * aspect / CCV_DPM_WINDOW_SIZE + 0.5), 1);
		rows[i] = ccv_max((int)(sqrtf(area / aspect) / CCV_DPM_WINDOW_SIZE + 0.5), 1);

		if (i < params.components - 1)
			PRINT(CCV_CLI_INFO, "%dx%d, ", cols[i], rows[i]);
		else
			PRINT(CCV_CLI_INFO, "%dx%d\n", cols[i], rows[i]);
		fflush(stdout);

		// 计算总的输入正样本数innum
		innum += mnum[i];
	}
	
	ccfree(fn);
	int corrupted = 1;
	
	for (i = 0; i < params.components; i++)
		if (model->root[i].root.w)
		{
			PRINT(CCV_CLI_INFO, "skipping root mixture model initialization for model %d(%d)\n", i + 1, params.components);
			corrupted = 0;
		} else
			break;
		
	if (corrupted)
	{
		PRINT(CCV_CLI_INFO, "root mixture model initialization corrupted, reboot\n");
		ccv_array_t** posex = (ccv_array_t**)alloca(sizeof(ccv_array_t*) * params.components);

		// Data 1: Positive Examples P = {(I1, B1), ..., (In, Bn)} 
		for (i = 0; i < params.components; i++)
		{
			// 根据矩形包围框召集正样本
			// posnum从上层传入的实际读到的正样本文件数
			posex[i] = _ccv_dpm_summon_examples_by_rectangle(posfiles, 
															 bboxes, 
															 posnum, 
															 i, 
															 rows[i], 
															 cols[i], 
															 params.grayscale);
		}
		
		PRINT(CCV_CLI_INFO, "\n");

		ccv_array_t** negex = (ccv_array_t**)alloca(sizeof(ccv_array_t*) * params.components);

		// 随机收集负样本
		// Data 2: Negitive Images N = {J1, ... Jm} 
		_ccv_dpm_collect_examples_randomly(rng, 
										   negex, 
										   bgfiles, 
										   bgnum, 
										   negnum, 
										   params.components, 
										   rows, 
										   cols, 
										   params.grayscale);
		
		PRINT(CCV_CLI_INFO, "\n");
		int* neglabels = (int*)ccmalloc(sizeof(int) * negex[0]->rnum);

		for (i = 0; i < negex[0]->rnum; i++)
		{
			// 给负样本标签数组随机分配组件号
			neglabels[i] = gsl_rng_uniform_int(rng, params.components);
		}

		// 按组件数循环
		for (i = 0; i < params.components; i++)
		{
			// 获取当前根分类器指针
			ccv_dpm_root_classifier_t* root_classifier = model->root + i;

			// 创建当前根分类器的根w矩阵
			// rows,cols根据前1/5正样本的最大长宽来计算DPM扫描块矩阵的行列
			root_classifier->root.w = ccv_dense_matrix_new(rows[i], cols[i], CCV_32F | 31, 0, 0);
			PRINT(CCV_CLI_INFO, "initializing root mixture model for model %d(%d)\n", i + 1, params.components);

			// 根据已有的正负样本训练初始模型输出到root_classifier->root.w,root_classifier->beta
			// 初始化当前根分类器
			// Data 3: Initial model Beta
			_ccv_dpm_initialize_root_classifier(rng, 
												root_classifier, 
												i, 
												mnum[i], 
												poslabels, 
												posex[i], 
												neglabels, 
												negex[i], 
												params.C, 
												params.symmetric, 
												params.grayscale);
		}

		ccfree(neglabels);
		ccfree(poslabels);

		// 检测产生出来的根特征的对称属性
		// check symmetric property of generated root feature
		if (params.symmetric)
			for (i = 0; i < params.components; i++)
			{
				ccv_dpm_root_classifier_t* root_classifier = model->root + i;

				// 检查根分类器的对称性,如果不对称则打印输出symmetric violation
				_ccv_dpm_check_root_classifier_symmetry(root_classifier->root.w);
			}

		// 现在不知道部件，也不知道滤波器，没有滤波器就没有部件，没有部件也求不
		// 出滤波器的参数，这就是典型的EM算法要解决的事情，但是作者没有使用EM算
		// 法，而是使用隐SVM（Latent SVM）的方法，隐变量其实就是类似统计中的因
		// 子分析，在这里就是找到潜在部件。	
		// 如果部件数>1
		if (params.components > 1)
		{
			// lsvm的坐标下降
			/* TODO: coordinate-descent for lsvm */
			PRINT(CCV_CLI_INFO, "optimizing root mixture model with coordinate-descent approach\n");

			// 优化根混合模型 
			model = 
				_ccv_dpm_optimize_root_mixture_model(rng, 
													 model, 
													 posex, 
													 negex, 
													 params.root_relabels, 
													 params.balance, 
													 params.C, 
													 params.alpha, 
													 params.alpha_ratio, 
													 params.iterations, 
													 params.symmetric);
		} 
		else // 如果部件数==1
		{
			// 放弃用坐标下降法优化根混合模型	
			PRINT(CCV_CLI_INFO, "components == 1, skipped coordinate-descent to optimize root mixture model\n");
		}

		// 释放正负样本
		for (i = 0; i < params.components; i++)
		{
			for (j = 0; j < posex[i]->rnum; j++)
				_ccv_dpm_feature_vector_cleanup((ccv_dpm_feature_vector_t*)ccv_array_get(posex[i], j));

			ccv_array_free(posex[i]);

			for (j = 0; j < negex[i]->rnum; j++)
				_ccv_dpm_feature_vector_cleanup((ccv_dpm_feature_vector_t*)ccv_array_get(negex[i], j));

			ccv_array_free(negex[i]);
		}
	} 
	else 
	{
		ccfree(poslabels);
	}
	
	_ccv_dpm_write_checkpoint(model, 0, checkpoint);

	// 初始化部件滤波器
	/* initialize part filter */
	PRINT(CCV_CLI_INFO, "initializing part filters\n");
	
	for (i = 0; i < params.components; i++)
	{
		// 如果第i个组件的部件数>0
		if (model->root[i].count > 0)
		{
			// 如果前面已经初始化了部件分类器则不再初始化
			PRINT(CCV_CLI_INFO, " - skipping part filters initialization for model %d(%d)\n", i + 1, params.components);
		} 
		else // 如果第i个组件的部件数<=0
		{
			PRINT(CCV_CLI_INFO, " - initializing part filters for model %d(%d)\n", i + 1, params.components);

			// 初始化部件分类器
			_ccv_dpm_initialize_part_classifiers(model->root + i, 
												 params.parts, // --model-part 8
												 params.symmetric);

			// 写检查点
			_ccv_dpm_write_checkpoint(model, 0, checkpoint);
			_ccv_dpm_write_checkpoint(model, 0, initcheckpoint);
		}
	}
	
	_ccv_dpm_write_checkpoint(model, 0, checkpoint);

	// 用随机梯度下降法优化根滤波器和部件滤波器
	/* optimize both root filter and part filters with stochastic gradient descent */
	PRINT(CCV_CLI_INFO, "optimizing root filter & part filters with stochastic gradient descent\n");
	char gradient_progress_checkpoint[512];
	sprintf(gradient_progress_checkpoint, "%s/gradient_descent_progress", dir);
	char feature_vector_checkpoint[512];
	sprintf(feature_vector_checkpoint, "%s/positive_vectors", dir);
	char neg_vector_checkpoint[512];
	sprintf(neg_vector_checkpoint, "%s/negative_vectors", dir);
	ccv_dpm_feature_vector_t** posv = (ccv_dpm_feature_vector_t**)ccmalloc(posnum * sizeof(ccv_dpm_feature_vector_t*));

	// 每幅图像最大负样本魔术数
	int* order = (int*)ccmalloc(sizeof(int) * (posnum + params.negative_cache_size + 64 /* the magical number for maximum negative examples collected per image */));
	double previous_positive_loss = 0, previous_negative_loss = 0, positive_loss = 0, negative_loss = 0, loss = 0;

	// 需要对每个样本重新赋给权重
	// need to re-weight for each examples
	c = d = t = 0;
	ccv_array_t* negv = 0;

	// 从文件neg_vector_checkpoint中读取负样本特征向量到negv
	if (0 == _ccv_dpm_read_negative_feature_vectors(&negv, 
													params.negative_cache_size, 
													neg_vector_checkpoint))
	{
		PRINT(CCV_CLI_INFO, " - read collected negative responses from last interrupted process\n");
	}

	// 从文件gradient_progress_checkpoint读取梯度下降过程到c,d
	_ccv_dpm_read_gradient_descent_progress(&c, &d, gradient_progress_checkpoint);

	// 2: for relabel := 1 to num-relabel do 
	// root_relabels = 20
	for (; c < params.relabels; c++)
	{
		// 获取惩罚因子到regz_rate	
		double regz_rate = params.C;

		// 3: Fp := 0
		ccv_dpm_mixture_model_t* _model;

		// 3-6就对于MI-SVM的第一步:在正样本集中优化
		// 从文件feature_vector_checkpoint中读取正样本特征向量到posv
		if (0 == _ccv_dpm_read_positive_feature_vectors(posv, posnum, feature_vector_checkpoint))
		{
			PRINT(CCV_CLI_INFO, " - read collected positive responses from last interrupted process\n");
		} 
		else 
		{
			FLUSH(CCV_CLI_INFO, " - collecting responses from positive examples : 0%%");

			// 4: for i := 1 to n do
			for (i = 0; i < posnum; i++)
			{
				FLUSH(CCV_CLI_INFO, " - collecting responses from positive examples : %d%%", i * 100 / posnum);
				ccv_dense_matrix_t* image = 0;

				// 将文件posfiles[i]读到image里
				ccv_read(posfiles[i], &image, (params.grayscale ? CCV_IO_GRAY : 0) | CCV_IO_ANY_FILE);

				// 5: Add detect-best(Beta, Ii, Bi) to Fp
				// 收集最好的正样本到正样本特征向量集posv
				posv[i] = _ccv_dpm_collect_best(image, 
												model, 
												bboxes[i], 
												params.include_overlap, 
												params.detector);
				ccv_matrix_free(image);
			} // 6: end
			
			FLUSH(CCV_CLI_INFO, " - collecting responses from positive examples : 100%%\n");

			// 将已收集好的正样本特征向量集写到文件feature_vector_checkpoint里
			_ccv_dpm_write_positive_feature_vectors(posv, posnum, feature_vector_checkpoint);
		}
		
		int* posvnum = (int*)alloca(sizeof(int) * model->count);
		memset(posvnum, 0, sizeof(int) * model->count);
		
		for (i = 0; i < posnum; i++)
		{	
			if (posv[i])
			{
				assert(posv[i]->id >= 0 && posv[i]->id < model->count);
				++posvnum[posv[i]->id];
			}
		}
			
		PRINT(CCV_CLI_INFO, " - positive examples divided by components : %d", posvnum[0]);

		for (i = 1; i < model->count; i++)
			PRINT(CCV_CLI_INFO, ", %d", posvnum[i]);
		PRINT(CCV_CLI_INFO, "\n");
		params.detector.threshold = 0;

		// 数据挖掘主循环
		/* 现在说下Data-mining。作者为什么不直接优化，还搞个Data-minig干嘛呢?因
		为，负样本数目巨大，Version3中用到的总样本数为2^28，其中Pos样本数目占的
		比例特别低，负样本太多，直接导致优化过程很慢，因为很多负样本远离分界面
		对于优化几乎没有帮助。Data-mining的作用就是去掉那些对优化作用很小的
		Easy-examples保留靠近分界面的Hard-examples。分别对应13和10。*/
		// 数据挖掘:挖掘难例需要多少数据挖掘过程(默认到50) .data_minings = 50
		// data-minings : how many data mining procedures are needed for discovering hard examples [DEFAULT TO 50]
		// 7: for data_mining := 1 to num_mining do
		for (; d < params.data_minings; d++)
		{
			// cache用完，重新收集
			// the cache is used up now, collect again
			_ccv_dpm_write_gradient_descent_progress(c, d, gradient_progress_checkpoint);

			// 获取随机梯度下降的步长
			double alpha = params.alpha;

			// 如果负样本集negv不为空
			if (negv)
			{
				// 创建新样本集av
				ccv_array_t* av = ccv_array_new(sizeof(ccv_dpm_feature_vector_t*), 64, 0);

				// 按负样本数循环
				for (j = 0; j < negv->rnum; j++)
				{
					// 从负样本集negv中获取负样本向量v
					ccv_dpm_feature_vector_t* v = 
						*(ccv_dpm_feature_vector_t**)ccv_array_get(negv, j);

					// 计算负样本向量v的综合得分score
					double score = _ccv_dpm_vector_score(model, v);

					assert(!isnan(score));

					// ?如果靠近分界面
					if (score >= -1)
					{
						// 将靠近分界面的Hard-examples压入av
						ccv_array_push(av, &v);
					}
					else
					{
						_ccv_dpm_feature_vector_free(v);
					}
				}
				
				ccv_array_free(negv);
				negv = av;
			} 
			else // 如果负样本集negv为空
			{
				// 创建sizeof(ccv_dpm_feature_vector_t*) * 64大小的负样本集
				negv = ccv_array_new(sizeof(ccv_dpm_feature_vector_t*), 64, 0);
			}
			
			FLUSH(CCV_CLI_INFO, " - collecting negative examples -- (0%%)");

			// 如果负样本数组容量小于负样本缓冲区大小
			if (negv->rnum < params.negative_cache_size)
			{
				// 从背景文件bgfiles收集负样本到负样本集negv
				// 8~11 8: for j := 1 to m do 
				_ccv_dpm_collect_from_background(negv, rng, bgfiles, bgnum, model, params, 0);
			}
			
			// 将负样本特征向量集negv写到文件neg_vector_checkpoint
			_ccv_dpm_write_negative_feature_vectors(negv, params.negative_cache_size, neg_vector_checkpoint);
			FLUSH(CCV_CLI_INFO, " - collecting negative examples -- (100%%)\n");
			int* negvnum = (int*)alloca(sizeof(int) * model->count);
			memset(negvnum, 0, sizeof(int) * model->count);

			for (i = 0; i < negv->rnum; i++)
			{
				ccv_dpm_feature_vector_t* v = *(ccv_dpm_feature_vector_t**)ccv_array_get(negv, i);
				assert(v->id >= 0 && v->id < model->count);
				++negvnum[v->id];
			}

			// 如果负样本数小于负样本缓冲区大小的一半或者REGQ, MINI_BATCH
			if (negv->rnum <= ccv_max(params.negative_cache_size / 2, ccv_max(REGQ, MINI_BATCH)))
			{
				for (i = 0; i < model->count; i++)
				{
					// 没有获取到足够的负样本，则调整常数并终止下一次循环	
					// we cannot get sufficient negatives, adjust constant and abort for next round
					_ccv_dpm_adjust_model_constant(model, i, posv, posnum, params.percentile_breakdown);
				}
				
				continue;
			}
			
			PRINT(CCV_CLI_INFO, " - negative examples divided by components : %d", negvnum[0]);

			for (i = 1; i < model->count; i++)
				PRINT(CCV_CLI_INFO, ", %d", negvnum[i]);
			
			PRINT(CCV_CLI_INFO, "\n");
			previous_positive_loss = previous_negative_loss = 0;
			uint64_t elapsed_time = _ccv_dpm_time_measure();
			assert(negv->rnum < params.negative_cache_size + 64);

			// 12就对应了MI-SVM的第二步:优化SVM模型
			// 直接用了梯度下降法，求解最优模型β
			// 12: Beta := gradient-descent(Fp union Fn) 
			// iterations 500 1000 50000
			for (t = 0; t < params.iterations; t++)
			{
				// model->count == params.components
				for (p = 0; p < model->count; p++)
				{
					// 如果没有足够的正负样本，则终止本次循环	
					// if don't have enough negnum or posnum, aborting
					if (negvnum[p] <= ccv_max(params.negative_cache_size / (model->count * 3), ccv_max(REGQ, MINI_BATCH)) 
					 ||	posvnum[p] <= ccv_max(REGQ, MINI_BATCH))
					{
						continue;
					}

					// 计算正负样本权重
					double pos_weight = sqrt((double)negvnum[p] / posvnum[p] * params.balance); // positive weight
					double neg_weight = sqrt((double)posvnum[p] / negvnum[p] / params.balance); // negative weight
					_model = _ccv_dpm_model_copy(model);
					
					for (i = 0; i < posnum + negv->rnum; i++)
						order[i] = i;

					// 把order打乱成随机序列
					gsl_ran_shuffle(rng, order, posnum + negv->rnum, sizeof(int));
					int l = 0;

					// 按样本总数循环
					for (i = 0; i < posnum + negv->rnum; i++)
					{
						// 随机取一个样本序号
						k = order[i];

						// 如果是正样本
						if (k < posnum)
						{
							if (posv[k] == 0 || posv[k]->id != p)
								continue;

							// 计算小批方法的损失
							double score = _ccv_dpm_vector_score(model, posv[k]); // the loss for mini-batch method (computed on model)
							assert(!isnan(score));

							// 12: Beta := gradient-descent(Fp)
							if (score <= 1)
							{
								_ccv_dpm_stochastic_gradient_descent(_model, 
																	 posv[k], 
																	 1, 
																	 alpha * pos_weight, 
																	 regz_rate, 
																	 params.symmetric);
							}
						} 
						else // 如果是负样本
						{
							// 获取当前负样本特征向量v
							ccv_dpm_feature_vector_t* v = 
								*(ccv_dpm_feature_vector_t**)ccv_array_get(negv, k - posnum);

							if (v->id != p)
								continue;

							// 是rootfilter(主模型)的得分，或者说是匹配程度，本
							// 质就是Beta和Feature的卷积
							double score = _ccv_dpm_vector_score(model, v);
							
							assert(!isnan(score));

							// 12: Beta := gradient-descent(Fn)
							if (score >= -1)
							{
								_ccv_dpm_stochastic_gradient_descent(_model, 
																	 v, 
																	 -1, 
																	 alpha * neg_weight, 
																	 regz_rate, 
																	 params.symmetric);
							}
						}

						// 已计算的样本数+1
						++l;

						// 每循环REGQ - 1次做一次规则化混合模型
						if (l % REGQ == REGQ - 1)
						{
							// 用第3个参数归一化根分类器的w和beta,部件分类器的w,dx,dy,dxx,dyy	
							_ccv_dpm_regularize_mixture_model(
								_model, 
								p, 
								1.0 - pow(1.0 - alpha / (double)((posvnum[p] + negvnum[p]) * (!!params.symmetric + 1)), REGQ));
						}

						// 每循环MINI_BATCH - 1次做一次释放
						if (l % MINI_BATCH == MINI_BATCH - 1)
						{
							// mimicking mini-batch way of doing things
							_ccv_dpm_mixture_model_cleanup(model);
							
							ccfree(model);
							model = _model;
							_model = _ccv_dpm_model_copy(model);
						}
					}

					// 处理完所有样本过后再做一次规则化混合模型
					// (ccv_dpm_mixture_model_t* model, int i, double regz)
					_ccv_dpm_regularize_mixture_model(_model, 
												      p, 
												      1.0 - pow(1.0 - alpha / (double)((posvnum[p] + negvnum[p]) * (!!params.symmetric + 1)), (((posvnum[p] + negvnum[p]) % REGQ) + 1) % (REGQ + 1)));

					_ccv_dpm_mixture_model_cleanup(model);
					ccfree(model);
					model = _model;
				}

				// 计算损失
				// compute the loss
				int posvn = 0;
				positive_loss = negative_loss = loss = 0;

				// 计算正样本的加权铰链损失loss
				for (i = 0; i < posnum; i++)
				{
					if (posv[i] != 0)
					{
						double score = _ccv_dpm_vector_score(model, posv[i]);
						assert(!isnan(score));
						double hinge_loss = ccv_max(0, 1.0 - score);
						positive_loss += hinge_loss;
						double pos_weight = 
							sqrt((double)negvnum[posv[i]->id] / posvnum[posv[i]->id] * params.balance); // positive weight
						loss += pos_weight * hinge_loss;
						++posvn;
					}
				}

				// 计算负样本的加权铰链损失loss
				for (i = 0; i < negv->rnum; i++)
				{
					ccv_dpm_feature_vector_t* v = *(ccv_dpm_feature_vector_t**)ccv_array_get(negv, i);
					double score = _ccv_dpm_vector_score(model, v);
					assert(!isnan(score));
					double hinge_loss = ccv_max(0, 1.0 + score);
					negative_loss += hinge_loss;
					double neg_weight = 
						sqrt((double)posvnum[v->id] / negvnum[v->id] / params.balance); // negative weight
					loss += neg_weight * hinge_loss;
				}

				// 计算所有样本的加权平均铰链损失loss
				loss = loss / (posvn + negv->rnum);

				// 计算正样本的加权平均铰链损失positive_loss
				positive_loss = positive_loss / posvn;

				// 计算负样本的加权平均铰链损失negative_loss
				negative_loss = negative_loss / negv->rnum;
				
				FLUSH(CCV_CLI_INFO, " - with loss %.5lf (positive %.5lf, negative %.5f) at rate %.5lf %d | %d -- %d%%", loss, positive_loss, negative_loss, alpha, posvn, negv->rnum, (t + 1) * 100 / params.iterations);

				// 检查已产生的根特征的对称属性
				// check symmetric property of generated root feature
				if (params.symmetric)
					for (i = 0; i < params.components; i++)
					{
						ccv_dpm_root_classifier_t* root_classifier = model->root + i;
						_ccv_dpm_check_root_classifier_symmetry(root_classifier->root.w);
					}

				// 如果正负样本的平均铰链损失都接近上一次的值则退出迭代	
				if (fabs(previous_positive_loss - positive_loss) < 1e-5 &&
					fabs(previous_negative_loss - negative_loss) < 1e-5)
				{
					PRINT(CCV_CLI_INFO, "\n - aborting iteration at %d because we didn't gain much", t + 1);
					break;
				}

				// 保存到上一次的铰链损失
				previous_positive_loss = positive_loss;
				previous_negative_loss = negative_loss;

				// 每次迭代它会减少
				alpha *= params.alpha_ratio; // it will decrease with each iteration
			}

			// 将model保存到checkpoint文件里面
			_ccv_dpm_write_checkpoint(model, 0, checkpoint);
			PRINT(CCV_CLI_INFO, "\n - data mining %d takes %.2lf seconds at loss %.5lf, %d more to go (%d of %d)\n", d + 1, (double)(_ccv_dpm_time_measure() - elapsed_time) / 1000000.0, loss, params.data_minings - d - 1, c + 1, params.relabels);
			j = 0;
			double* scores = (double*)ccmalloc(posnum * sizeof(double));

			// 计算所有正样本的得分，放到scores
			for (i = 0; i < posnum; i++)
			{
				if (posv[i])
				{
					// 计算posv[i]跟根滤波器和所有部件滤波器的响应分数
					scores[j] = _ccv_dpm_vector_score(model, posv[i]);

					// 如果是数字
					assert(!isnan(scores[j]));
					j++;
				}
			}

			// 排序分数完了就释放，似乎没有用上	
			_ccv_dpm_score_qsort(scores, j, 0);
			ccfree(scores);
			double breakdown;
			PRINT(CCV_CLI_INFO, " - threshold breakdown by percentile");

			// 百分率分解，默认阈值0.05
			for (breakdown = params.percentile_breakdown; 
				 breakdown < 1.0; 
				 breakdown += params.percentile_breakdown)
				PRINT(CCV_CLI_INFO, " %0.2lf(%.1f%%)", scores[ccv_clamp((int)(breakdown * j), 0, j - 1)], (1.0 - breakdown) * 100);

			PRINT(CCV_CLI_INFO, "\n");
			char persist[512];
			sprintf(persist, "%s/model.%d.%d", dir, c, d);
			_ccv_dpm_write_checkpoint(model, 0, persist);
		}
		
		d = 0;

		// 如果终止，则表明不能找到足够的负样本，尝试调整常数
		// if abort, means that we cannot find enough negative examples, try to adjust constant
		for (i = 0; i < posnum; i++)
			if (posv[i])
				_ccv_dpm_feature_vector_free(posv[i]);
			
		remove(feature_vector_checkpoint);
	}

	// 如果负样本集不为空则释放负样本集
	if (negv)
	{
		for (i = 0; i < negv->rnum; i++)
		{
			ccv_dpm_feature_vector_t* v = *(ccv_dpm_feature_vector_t**)ccv_array_get(negv, i);
			_ccv_dpm_feature_vector_free(v);
		}
		
		ccv_array_free(negv);
	}
	
	remove(neg_vector_checkpoint);
	ccfree(order);
	ccfree(posv);

	// 用线性回归预测根矩形
	PRINT(CCV_CLI_INFO, "root rectangle prediction with linear regression\n");

	// 后处理 包围盒预测
	// 预测新的x, y和scale
	// Result: New model Beta
	_ccv_dpm_initialize_root_rectangle_estimator(model, posfiles, bboxes, posnum, params);

	_ccv_dpm_write_checkpoint(model, 1, checkpoint);
	PRINT(CCV_CLI_INFO, "done\n");
	remove(gradient_progress_checkpoint);
	_ccv_dpm_mixture_model_cleanup(model);
	ccfree(model);
	gsl_rng_free(rng);
}
#else
void ccv_dpm_mixture_model_new(char** posfiles, ccv_rect_t* bboxes, int posnum, char** bgfiles, int bgnum, int negnum, const char* dir, ccv_dpm_new_param_t params)
{
	fprintf(stderr, " ccv_dpm_classifier_cascade_new requires libgsl and liblinear support, please compile ccv with them.\n");
}
#endif
#else
void ccv_dpm_mixture_model_new(char** posfiles, ccv_rect_t* bboxes, int posnum, char** bgfiles, int bgnum, int negnum, const char* dir, ccv_dpm_new_param_t params)
{
	fprintf(stderr, " ccv_dpm_classifier_cascade_new requires libgsl and liblinear support, please compile ccv with them.\n");
}
#endif

static int _ccv_is_equal(const void* _r1, const void* _r2, void* data)
{
	const ccv_root_comp_t* r1 = (const ccv_root_comp_t*)_r1;
	const ccv_root_comp_t* r2 = (const ccv_root_comp_t*)_r2;
	int distance = (int)(ccv_min(r1->rect.width, r1->rect.height) * 0.25 + 0.5);

	return r2->rect.x <= r1->rect.x + distance &&
		r2->rect.x >= r1->rect.x - distance &&
		r2->rect.y <= r1->rect.y + distance &&
		r2->rect.y >= r1->rect.y - distance &&
		r2->rect.width <= (int)(r1->rect.width * 1.5 + 0.5) &&
		(int)(r2->rect.width * 1.5 + 0.5) >= r1->rect.width &&
		r2->rect.height <= (int)(r1->rect.height * 1.5 + 0.5) &&
		(int)(r2->rect.height * 1.5 + 0.5) >= r1->rect.height;
}

static int _ccv_is_equal_same_class(const void* _r1, const void* _r2, void* data)
{
	const ccv_root_comp_t* r1 = (const ccv_root_comp_t*)_r1;
	const ccv_root_comp_t* r2 = (const ccv_root_comp_t*)_r2;
	int distance = (int)(ccv_min(r1->rect.width, r1->rect.height) * 0.25 + 0.5);

	return r2->classification.id == r1->classification.id &&
		r2->rect.x <= r1->rect.x + distance &&
		r2->rect.x >= r1->rect.x - distance &&
		r2->rect.y <= r1->rect.y + distance &&
		r2->rect.y >= r1->rect.y - distance &&
		r2->rect.width <= (int)(r1->rect.width * 1.5 + 0.5) &&
		(int)(r2->rect.width * 1.5 + 0.5) >= r1->rect.width &&
		r2->rect.height <= (int)(r1->rect.height * 1.5 + 0.5) &&
		(int)(r2->rect.height * 1.5 + 0.5) >= r1->rect.height;
}

// 在一幅给定的图像里面用DPM模型来检测目标
// 如果有几个DPM混合模型，则最好是在一个函数调用中使用它们。
// 这样，CCV将尝试优化整体性能
/*
a: The input image.
model: An array of mixture models.
count: How many mixture models you’ve passed in.
params: A ccv_dpm_param_t structure that defines various aspects of the detector.

return: A ccv_array_t of ccv_root_comp_t that contains the root bounding box as 
well as its parts.
*/
ccv_array_t* ccv_dpm_detect_objects(ccv_dense_matrix_t* a, 
									ccv_dpm_mixture_model_t** _model, 
									int count, 
									ccv_dpm_param_t params)
{
	int c, i, j, k, x, y;

	// .interval = 8, .min_neighbors = 1, .flags = 0, .threshold = 0.6, // 0.8
	double scale = pow(2.0, 1.0 / (params.interval + 1.0));
	int next = params.interval + 1;

	// count = 1
	int scale_upto = _ccv_dpm_scale_upto(a, _model, count, params.interval);

	// 图像太小则返回
	if (scale_upto < 0) // image is too small to be interesting
		return 0;

	// 根据输入图像a生成DPM特征金字塔pyr
	ccv_dense_matrix_t** pyr = 
		(ccv_dense_matrix_t**)alloca((scale_upto + next * 2) * sizeof(ccv_dense_matrix_t*));
	_ccv_dpm_feature_pyramid(a, pyr, scale_upto, params.interval);

	ccv_array_t* idx_seq;
	ccv_array_t* seq = ccv_array_new(sizeof(ccv_root_comp_t), 64, 0);
	ccv_array_t* seq2 = ccv_array_new(sizeof(ccv_root_comp_t), 64, 0);
	ccv_array_t* result_seq = ccv_array_new(sizeof(ccv_root_comp_t), 64, 0);

	// 按组件数循环
	for (c = 0; c < count; c++)
	{
		// 获取第c个_model
		ccv_dpm_mixture_model_t* model = _model[c];
		double scale_x = 1.0;
		double scale_y = 1.0;

		// next == 9
		for (i = next; i < scale_upto + next * 2; i++)
		{
			// 按模型数循环
			for (j = 0; j < model->count; j++)
			{
				// 获取第j个根分类器指针root
				ccv_dpm_root_classifier_t* root = model->root + j;
				ccv_dense_matrix_t* root_feature = 0;
				ccv_dense_matrix_t* part_feature[CCV_DPM_PART_MAX];
				ccv_dense_matrix_t* dx[CCV_DPM_PART_MAX];
				ccv_dense_matrix_t* dy[CCV_DPM_PART_MAX];

				// 计算综合得分score(x0, y0, l0)放到&root_feature,part_feature里
				_ccv_dpm_compute_score(root, 
									   pyr[i], 
									   pyr[i - next], 
									   &root_feature, 
									   part_feature, 
									   dx, 
									   dy);

				// 计算根分类器w行列数的一半rwh,rww,rwh_1,rww_1
				int rwh = (root->root.w->rows - 1) / 2, rww = (root->root.w->cols - 1) / 2;
				int rwh_1 = root->root.w->rows / 2, rww_1 = root->root.w->cols / 2;

				// 从root_feature里获取rwh行0列的数据指针到f_ptr
				/* 这些数值设计来确保根分类器奇偶行列的有效性:
				假设图像是6x6，根分类器也是6x6，则扫描区域应该从(2,2)到(2,2),
				因此，它通过(rwh, rww)到(6 - rwh_1 - 1, 6 - rww_1 - 1)
				这个计算对奇数根分类器也有效
				*/
				/* these values are designed to make sure works with odd/even number of rows/cols
				 * of the root classifier:
				 * suppose the image is 6x6, and the root classifier is 6x6, the scan area should starts
				 * at (2,2) and end at (2,2), thus, it is capped by (rwh, rww) to (6 - rwh_1 - 1, 6 - rww_1 - 1)
				 * this computation works for odd root classifier too (i.e. 5x5) */
				float* f_ptr = (float*)ccv_get_dense_matrix_cell_by(CCV_32F | CCV_C1, root_feature, rwh, 0, 0);

				for (y = rwh; y < root_feature->rows - rwh_1; y++)
				{
					for (x = rww; x < root_feature->cols - rww_1; x++)
					{
						// 设定一个阈值，窗口分数高于阈值的则判别为目标。 
						// 如果组件置信度(根特征值 + 偏移) > 阈值0.6
						if (f_ptr[x] + root->beta > params.threshold)
						{
							ccv_root_comp_t comp;
							comp.neighbors = 1;

							// 设置组件类别为c + 1
							comp.classification.id = c + 1;

							// 设置组件置信度为f_ptr[x] + root->beta
							comp.classification.confidence = f_ptr[x] + root->beta;

							// 设置组件的部件数
							comp.pnum = root->count;

							// 获取根部件的x,y漂移和尺度
							float drift_x = root->alpha[0],
								  drift_y = root->alpha[1],
								  drift_scale = root->alpha[2];

							// 按部件分类器个数循环 
							for (k = 0; k < root->count; k++)
							{
								// 获取第k个部件分类器
								ccv_dpm_part_classifier_t* part = root->part + k;
								comp.part[k].neighbors = 1;

								// 设置部件k类别为c
								comp.part[k].classification.id = c;

								// 计算部件行列数的一半
								int pww = (part->w->cols - 1) / 2, pwh = (part->w->rows - 1) / 2;

								// 计算部件偏移
								int offy = part->y + pwh - rwh * 2;
								int offx = part->x + pww - rww * 2;
								int iy = ccv_clamp(y * 2 + offy, pwh, part_feature[k]->rows - part->w->rows + pwh);
								int ix = ccv_clamp(x * 2 + offx, pww, part_feature[k]->cols - part->w->cols + pww);

								// 从广义距离dy,dx里获取对应的元素ry,rx
								int ry = 
									ccv_get_dense_matrix_cell_value_by(CCV_32S | CCV_C1, dy[k], iy, ix, 0);
								int rx = 
									ccv_get_dense_matrix_cell_value_by(CCV_32S | CCV_C1, dx[k], iy, ix, 0);

								// 累加部件k的漂移到组件漂移
								// di dp Phi_d(dx, dy)即为最普遍的欧氏距离
								drift_x += part->alpha[0] * rx + part->alpha[1] * ry;
								drift_y += part->alpha[2] * rx + part->alpha[3] * ry;

								// 累加部件k的尺度到组件尺度
								drift_scale += part->alpha[4] * rx + part->alpha[5] * ry;
								ry = iy - ry;
								rx = ix - rx;

								// 计算部件k的包围框
								comp.part[k].rect = 
									ccv_rect((int)((rx - pww) * CCV_DPM_WINDOW_SIZE / 2 * scale_x + 0.5), 
											 (int)((ry - pwh) * CCV_DPM_WINDOW_SIZE / 2 * scale_y + 0.5), 
											 (int)(part->w->cols * CCV_DPM_WINDOW_SIZE / 2 * scale_x + 0.5), 
											 (int)(part->w->rows * CCV_DPM_WINDOW_SIZE / 2 * scale_y + 0.5));

								// 获取部件k的置信度
								comp.part[k].classification.confidence = 
									-ccv_get_dense_matrix_cell_value_by(CCV_32F | CCV_C1, part_feature[k], iy, ix, 0);
							}

							// 计算组件的包围框
							comp.rect = 
								ccv_rect((int)(
								(x + drift_x) * CCV_DPM_WINDOW_SIZE * scale_x - rww * CCV_DPM_WINDOW_SIZE * scale_x * (1.0 + drift_scale) + 0.5), 
								(int)((y + drift_y) * CCV_DPM_WINDOW_SIZE * scale_y - rwh * CCV_DPM_WINDOW_SIZE * scale_y * (1.0 + drift_scale) + 0.5), 
								(int)(root->root.w->cols * CCV_DPM_WINDOW_SIZE * scale_x * (1.0 + drift_scale) + 0.5), 
								(int)(root->root.w->rows * CCV_DPM_WINDOW_SIZE * scale_y * (1.0 + drift_scale) + 0.5));

							// 将该组件压栈到seq
							ccv_array_push(seq, &comp);
						}
					}
					
					f_ptr += root_feature->cols;
				}

				// 释放部件特征向量和广义距离
				for (k = 0; k < root->count; k++)
				{
					ccv_matrix_free(part_feature[k]);
					ccv_matrix_free(dx[k]);
					ccv_matrix_free(dy[k]);
				}
				
				ccv_matrix_free(root_feature);
			}
			
			scale_x *= scale;
			scale_y *= scale;
		}

		/*Dollar等人在完整的图像中检测目标时，在图像多尺度空间中采用滑动窗口法，
		其中滑动窗口步长为４个像素，尺度步长为２＾(1/10)。文中作者用到的简化
		ＮＭＳ只有一个参数，即重叠率阈值(overlap threshold)，并将其设置为化0.6。
		同样地，文献[49]中，Enzweiler等人将简化非极大值抑制的重叠率阈值设为0.5。
			在DPM目标检测算法中，Felzenszwalb等人较为详细地描述简化非极大值抑制
		的过程:首先分类器对图片完成检测后，获得检测结果集合D，其中每一个元素由1
		个检测边界框和相应的分类器得分构成；然后严格按分类器得分对集合D排序，选
		中得分最高的检测结果，依次计算该检测边界框对得分较低的候选边界框的覆盖
		比例，丢弃覆盖超过50%的候选边界框；再更新检测结果排序，重复上一个步骤，
		最终选择出所有符合要求的边界框。
		*/
		// 下面的代码是取自OpenCV的HAAR特征实现
		/* the following code from OpenCV's haar feature implementation */
		// 如果最小邻居数为0
		if (params.min_neighbors == 0)
		{
			// 按序列的元素个数循环
			for (i = 0; i < seq->rnum; i++)
			{
				ccv_root_comp_t* comp = (ccv_root_comp_t*)ccv_array_get(seq, i);
				ccv_array_push(result_seq, comp);
			}
		} 
		else // 如果最小邻居数不为0
		{
			idx_seq = 0;
			ccv_array_clear(seq2);

			// 分组已获取的矩形以滤除噪声
			// group retrieved rectangles in order to filter out noise
			int ncomp = ccv_array_group(seq, &idx_seq, _ccv_is_equal_same_class, 0);
			ccv_root_comp_t* comps = (ccv_root_comp_t*)ccmalloc((ncomp + 1) * sizeof(ccv_root_comp_t));
			memset(comps, 0, (ncomp + 1) * sizeof(ccv_root_comp_t));

			// ?简化非极大值抑制
			// 邻居数目计数
			// count number of neighbors
			for (i = 0; i < seq->rnum; i++)
			{
				ccv_root_comp_t r1 = *(ccv_root_comp_t*)ccv_array_get(seq, i);
				int idx = *(int*)ccv_array_get(idx_seq, i);

				comps[idx].classification.id = r1.classification.id;
				comps[idx].pnum = r1.pnum;

				// 如果当前目标框的置信度大于当前组件的置信度并且当前组件的邻居数为0
				if (r1.classification.confidence > comps[idx].classification.confidence 
				 || comps[idx].neighbors == 0)
				{
					// 设置当前组件的目标框和置信度
					comps[idx].rect = r1.rect;
					comps[idx].classification.confidence = r1.classification.confidence;

					// 拷贝部件到当前组件
					memcpy(comps[idx].part, r1.part, sizeof(ccv_comp_t) * CCV_DPM_PART_MAX);
				}

				// 相应组件的邻居数+1
				++comps[idx].neighbors;
			}

			// 计算平均包围盒
			// calculate average bounding box
			for (i = 0; i < ncomp; i++)
			{
				int n = comps[i].neighbors;

				// 如果当前组件的邻居数>=最小邻居数
				if (n >= params.min_neighbors)
				{
					// 将当前组件压栈
					ccv_array_push(seq2, comps + i);
				}
			}

			// 过滤出包含小目标矩形的大目标矩形
			// filter out large object rectangles contains small object rectangles
			for (i = 0; i < seq2->rnum; i++)
			{
				ccv_root_comp_t* r2 = (ccv_root_comp_t*)ccv_array_get(seq2, i);
				int distance = (int)(ccv_min(r2->rect.width, r2->rect.height) * 0.25 + 0.5);

				for (j = 0; j < seq2->rnum; j++)
				{
					ccv_root_comp_t r1 = *(ccv_root_comp_t*)ccv_array_get(seq2, j);

					if (i != j &&
						abs(r1.classification.id) == r2->classification.id &&
						r1.rect.x >= r2->rect.x - distance &&
						r1.rect.y >= r2->rect.y - distance &&
						r1.rect.x + r1.rect.width <= r2->rect.x + r2->rect.width + distance &&
						r1.rect.y + r1.rect.height <= r2->rect.y + r2->rect.height + distance &&
						// if r1 (the smaller one) is better, mute r2
						(r2->classification.confidence <= r1.classification.confidence && r2->neighbors < r1.neighbors))
					{
						r2->classification.id = -r2->classification.id;
						break;
					}
				}
			}

			// 过滤出在大目标矩形里面的小目标矩形 
			// filter out small object rectangles inside large object rectangles
			for (i = 0; i < seq2->rnum; i++)
			{
				ccv_root_comp_t r1 = *(ccv_root_comp_t*)ccv_array_get(seq2, i);

				if (r1.classification.id > 0)
				{
					int flag = 1;

					for (j = 0; j < seq2->rnum; j++)
					{
						ccv_root_comp_t r2 = *(ccv_root_comp_t*)ccv_array_get(seq2, j);
						int distance = (int)(ccv_min(r2.rect.width, r2.rect.height) * 0.25 + 0.5);

						if (i != j &&
							r1.classification.id == abs(r2.classification.id) &&
							r1.rect.x >= r2.rect.x - distance &&
							r1.rect.y >= r2.rect.y - distance &&
							r1.rect.x + r1.rect.width <= r2.rect.x + r2.rect.width + distance &&
							r1.rect.y + r1.rect.height <= r2.rect.y + r2.rect.height + distance &&
							(r2.classification.confidence > r1.classification.confidence || r2.neighbors >= r1.neighbors))
						{
							flag = 0;
							break;
						}
					}

					if (flag)
						ccv_array_push(result_seq, &r1);
				}
			}
			
			ccv_array_free(idx_seq);
			ccfree(comps);
		}
	}

	// 释放金字塔
	for (i = 0; i < scale_upto + next * 2; i++)
		ccv_matrix_free(pyr[i]);

	ccv_array_free(seq);
	ccv_array_free(seq2);

	// 定义检测到的目标序列
	ccv_array_t* result_seq2;
	
	/* the following code from OpenCV's haar feature implementation */
	if (params.flags & CCV_DPM_NO_NESTED)
	{
		// 分配64个组件空间
		result_seq2 = ccv_array_new(sizeof(ccv_root_comp_t), 64, 0);
		idx_seq = 0;

		// 为了过滤掉噪声，分组已获得的矩形 
		// group retrieved rectangles in order to filter out noise
		int ncomp = ccv_array_group(result_seq, &idx_seq, _ccv_is_equal, 0);
		ccv_root_comp_t* comps = (ccv_root_comp_t*)ccmalloc((ncomp + 1) * sizeof(ccv_root_comp_t));
		memset(comps, 0, (ncomp + 1) * sizeof(ccv_root_comp_t));

		// 邻居数目计数 
		// count number of neighbors
		for(i = 0; i < result_seq->rnum; i++)
		{
			ccv_root_comp_t r1 = *(ccv_root_comp_t*)ccv_array_get(result_seq, i);
			int idx = *(int*)ccv_array_get(idx_seq, i);

			if (comps[idx].neighbors == 0 
			 || comps[idx].classification.confidence < r1.classification.confidence)
			{
				comps[idx].classification.confidence = r1.classification.confidence;
				comps[idx].neighbors = 1;
				comps[idx].rect = r1.rect;
				comps[idx].classification.id = r1.classification.id;
				comps[idx].pnum = r1.pnum;
				memcpy(comps[idx].part, r1.part, sizeof(ccv_comp_t) * CCV_DPM_PART_MAX);
			}
		}

		// 计算平均包围盒
		// calculate average bounding box
		for(i = 0; i < ncomp; i++)
			if(comps[i].neighbors)
				ccv_array_push(result_seq2, &comps[i]);

		ccv_array_free(result_seq);
		ccfree(comps);
	}
	else 
	{
		result_seq2 = result_seq;
	}

	// 返回检测到的目标序列
	return result_seq2;
}

// 从一个模型文件中读取DPM混合模型
/*
directory: The model file for DPM mixture model.
return: A DPM mixture model, 0 if no valid DPM mixture model available.
*/
ccv_dpm_mixture_model_t* ccv_dpm_read_mixture_model(const char* directory)
{
    //LogStart();LogProcess();LogEnd();
    printf("\nccv_dpm_read_mixture_model\n");fflush(stdout);


	FILE* r = fopen(directory, "r");
	if (r == 0)
		return 0;
	int count;
	char flag;//printf("fscanf c");
	fscanf(r, "%c", &flag);
	assert(flag == '.');
	fscanf(r, "%d", &count);
	ccv_dpm_root_classifier_t* root_classifier = (ccv_dpm_root_classifier_t*)ccmalloc(sizeof(ccv_dpm_root_classifier_t) * count);
	memset(root_classifier, 0, sizeof(ccv_dpm_root_classifier_t) * count);
	int i, j, k;
	size_t size = sizeof(ccv_dpm_mixture_model_t) + sizeof(ccv_dpm_root_classifier_t) * count;
	/* the format is easy, but I tried to copy all data into one memory region */
	for (i = 0; i < count; i++)
	{
		int rows, cols;
		fscanf(r, "%d %d", &rows, &cols);
		fscanf(r, "%f %f %f %f", &root_classifier[i].beta, &root_classifier[i].alpha[0], &root_classifier[i].alpha[1], &root_classifier[i].alpha[2]);
		root_classifier[i].root.w = ccv_dense_matrix_new(rows, cols, CCV_32F | 31, ccmalloc(ccv_compute_dense_matrix_size(rows, cols, CCV_32F | 31)), 0);
		size += ccv_compute_dense_matrix_size(rows, cols, CCV_32F | 31);
		for (j = 0; j < rows * cols * 31; j++)
			fscanf(r, "%f", &root_classifier[i].root.w->data.f32[j]);
		ccv_make_matrix_immutable(root_classifier[i].root.w);
		fscanf(r, "%d", &root_classifier[i].count);
		ccv_dpm_part_classifier_t* part_classifier = (ccv_dpm_part_classifier_t*)ccmalloc(sizeof(ccv_dpm_part_classifier_t) * root_classifier[i].count);
		size += sizeof(ccv_dpm_part_classifier_t) * root_classifier[i].count;
		for (j = 0; j < root_classifier[i].count; j++)
		{
			fscanf(r, "%d %d %d", &part_classifier[j].x, &part_classifier[j].y, &part_classifier[j].z);
			fscanf(r, "%lf %lf %lf %lf", &part_classifier[j].dx, &part_classifier[j].dy, &part_classifier[j].dxx, &part_classifier[j].dyy);
			fscanf(r, "%f %f %f %f %f %f", &part_classifier[j].alpha[0], &part_classifier[j].alpha[1], &part_classifier[j].alpha[2], &part_classifier[j].alpha[3], &part_classifier[j].alpha[4], &part_classifier[j].alpha[5]);
			fscanf(r, "%d %d %d", &rows, &cols, &part_classifier[j].counterpart);
			part_classifier[j].w = ccv_dense_matrix_new(rows, cols, CCV_32F | 31, ccmalloc(ccv_compute_dense_matrix_size(rows, cols, CCV_32F | 31)), 0);
			size += ccv_compute_dense_matrix_size(rows, cols, CCV_32F | 31);
			for (k = 0; k < rows * cols * 31; k++)
				fscanf(r, "%f", &part_classifier[j].w->data.f32[k]);
			ccv_make_matrix_immutable(part_classifier[j].w);
		}
		root_classifier[i].part = part_classifier;
	}
	fclose(r);
	unsigned char* m = (unsigned char*)ccmalloc(size);
	ccv_dpm_mixture_model_t* model = (ccv_dpm_mixture_model_t*)m;
	m += sizeof(ccv_dpm_mixture_model_t);
	model->count = count;
	model->root = (ccv_dpm_root_classifier_t*)m;
	m += sizeof(ccv_dpm_root_classifier_t) * model->count;
	memcpy(model->root, root_classifier, sizeof(ccv_dpm_root_classifier_t) * model->count);
	ccfree(root_classifier);
	for (i = 0; i < model->count; i++)
	{
		ccv_dpm_part_classifier_t* part_classifier = model->root[i].part;
		model->root[i].part = (ccv_dpm_part_classifier_t*)m;
		m += sizeof(ccv_dpm_part_classifier_t) * model->root[i].count;
		memcpy(model->root[i].part, part_classifier, sizeof(ccv_dpm_part_classifier_t) * model->root[i].count);
		ccfree(part_classifier);
	}
	for (i = 0; i < model->count; i++)
	{
		ccv_dense_matrix_t* w = model->root[i].root.w;
		model->root[i].root.w = (ccv_dense_matrix_t*)m;
		m += ccv_compute_dense_matrix_size(w->rows, w->cols, w->type);
		memcpy(model->root[i].root.w, w, ccv_compute_dense_matrix_size(w->rows, w->cols, w->type));
		model->root[i].root.w->data.u8 = (unsigned char*)(model->root[i].root.w + 1);
		ccfree(w);
		for (j = 0; j < model->root[i].count; j++)
		{
			w = model->root[i].part[j].w;
			model->root[i].part[j].w = (ccv_dense_matrix_t*)m;
			m += ccv_compute_dense_matrix_size(w->rows, w->cols, w->type);
			memcpy(model->root[i].part[j].w, w, ccv_compute_dense_matrix_size(w->rows, w->cols, w->type));
			model->root[i].part[j].w->data.u8 = (unsigned char*)(model->root[i].part[j].w + 1);
			ccfree(w);
		}
	}

    LogEnd();
	
	return model;
}

// 从DPM混合模型释放内存
// model: The DPM mixture model.
void ccv_dpm_mixture_model_free(ccv_dpm_mixture_model_t* model)
{
	ccfree(model);
}
